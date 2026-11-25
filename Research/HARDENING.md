# Hardening log

What each night audited, how hard it tried to break it, what it found, and
what it judged to be a genuine no-op rather than a gap. A target recorded as
clean here does not need re-auditing unless the code has changed since.
Newest first.

---

## 2026-11-25: the warp mapping, which decides where a warped clip reads from

### What is being looked at

`Source/WarpMap.h`, 296 lines, and `Tests/WarpMapTest.cpp`, 603 of them.
Never audited here before.

It is picked for what a fault in it would sound like. Every path that moves a
marker or a clip's left edge goes back through `normaliseWarpMarkers`, and the
invariant it is there to restore, sorted and strictly increasing in both beat
and source, is what makes the mapping monotonic and invertible. A list that
breaks it makes a clip read backwards, which is a stutter, and it makes the
clip's own length in beats meaningless. `warpSourceAtSegment` is the audio
thread's half of it.

Chosen over the one remaining JUCE free header with no test at all,
`Fx/Unit.h`, because that header is not reachable: nothing in the tree
includes it and `Fx/Plugin.h`, the wrapper it is written for, does not exist
yet. Observations about it are recorded at the end of this entry so the night
that builds on it does not start from nothing.

### Status

In progress. Mutation harness next.

---

## 2026-11-24: the modulation matrix, which decides what every patch is wired to

### What is being looked at

`Source/Synth/ModMatrix.h`, 548 lines, and `Tests/ModMatrixTest.cpp`, 745 of
them. Never audited here before, and the thinnest test to source ratio of
anything JUCE free that is not pure data.

It is picked for what a fault in it would do rather than for its size. A
routing is saved in an `.ahp` and in every PAN One patch, it is evaluated per
sample on the audio thread, and the whole argument for naming sources and
destinations rather than numbering them is that the failure mode here is a
patch that loads, plays, and is wrong. Nothing in it can fail loudly.

Started on the 21st and finished on the 24th: the first night got as far as
picking the target and was cut off before it broke anything.

### Mutation testing: 50 breakages, 37 caught

Each one compiled with the sanitizers exactly as `Tests/run.sh` has them, and
the harness verified that the unmutated header passes first. Several died on a
sanitizer rather than on a comparison, which is the usual reminder that a plain
`-O2` harness would have called them survivors.

| Where | Tried | Caught |
|---|---|---|
| `shaped`, the clamps and the four shapes | 10 | 10 |
| `evaluate` | 14 | 11 |
| `apply` | 6 | 5 |
| the id lookups | 7 | 6 |
| `Patch`: `add`, `clear`, `isWired`, `anyRouteTo` | 8 | 4 |
| the source and destination tables | 5 | 2 |

`shaped` is covered as well as a function that size can be. Every one of its
ten breakages died, including the two the file's own comments warn about:
folding a bipolar source up by taking its absolute value, and applying a
transform to a source that is already in that polarity.

The thirteen survivors are not spread evenly. Four of them are every single
breakage of `anyRouteTo`, and that is where the finding is.

### The fault: a row switched off still counted as a routing

`Patch::anyRouteTo` answered yes for a row whose own on/off control was off.

That function has exactly one caller, and it is the worst possible one to be
wrong for. `Synth/Voice.h` asks it whether anything drives the gate, and opens
the gate from the amplitude envelope when nothing does, so that an instrument
nobody has patched still makes a sound. A switched off row pointed at the gate
therefore took the envelope away and put the matrix sum in its place, and
`evaluate` skips a switched off row, so that sum is nothing. The gate sits in
series with everything the voice produces.

Measured through the real voice rather than read off the source, because the
size of it is the point. The patch is the same in all three renders, and the
routing is env 1 at full depth on the gate, which is what the default already
does, so the only difference between the first two is whether the row exists:

| | loudest tenth of a second |
|---|---|
| no gate routing at all | 0.160530 |
| the routing switched on | 0.160530 |
| the same routing switched off | 0.000000 |

Not quieter. Silent. And a row's on/off is one of the 240 saved parameters, so
a host can automate it: this is a thing that moves while somebody is playing,
and the only thing on screen to explain the silence would be a row they had
just turned off.

The fix is that `anyRouteTo` now applies the same three tests `evaluate`
applies, in the same order, so the two cannot disagree about which rows are
live. The bound on the source is part of that: a row pointing at a source this
version has never heard of drives nothing, and `evaluate` already read it that
way. `evaluate` itself was right throughout; every one of its breakages that
involved `enabled` died on the existing test.

### Why nothing had caught it

`anyRouteTo` had no check pointed at it. Not a weak one: none. The word
`anyRouteTo` did not appear in `ModMatrixTest.cpp`, which is why all four of
its breakages survived, and the one place that does exercise the behaviour
end to end, `VoiceTest::theGateDefaultYields`, builds its gate routings with
`Patch::add`, which leaves `enabled` at its default of true.

So the gap was not in the arithmetic anybody thought to check. It was a
function whose answer is a yes or a no, in a file where everything else
produces a number, and the numbers had all the attention.

### The second finding: a wider fuzz was a weaker one

`nothingInAPatchCanBreakIt` draws row indices from minus two thousand to two
thousand over twenty thousand trials, and it looks like thorough coverage of
the bounds. It is not. An off by one in a bound admits exactly one index, so
for a row to reach the source bound at all its source has to land on that one
value **and** its destination has to land anywhere inside the table at the
same time: two independent draws of about one in four thousand. That works out
at 0.69 rows in a whole run, and the destination bound at 0.47.

Which is to say the suite caught those two off by ones on a coin toss, and on
the run that matters it lost. Both survived. Narrowing the draw to plus or
minus forty killed both immediately, which is the measurement that settles it:

| index range | the source bound off by one | the destination bound off by one |
|---|---|---|
| plus or minus 2000 | survived | survived |
| plus or minus 40 | caught, on a sanitizer | caught, on a sanitizer |

**A wider fuzz is not a stronger one when what it has to hit is a single exact
value.** The range is drawn narrow four times in five now, with the wide draw
kept for what it is actually good at, which is arithmetic that overflows rather
than arithmetic that is off by one, and the exact edges are tried deliberately
in a check of their own rather than left to arrive.

The same gap, in a plainer form, is why the off by ones in `apply` and
`sourceId` survived: both are only ever called with hand picked indices, and
nobody had picked the first index past the end.

### The third finding: two columns of the tables that nothing read

The existing check on `apply` compares against the range it reads out of the
same table it is testing, so halving the cutoff's declared range survived the
whole file, and with it every saved patch's filter sweep. Declaring an envelope
bipolar survived too: it halves the movement of every routing from that
envelope that asked to be bipolar, and the envelope still looks and sounds like
an envelope.

Neither can be derived from anything, so both are now pinned by name, the way
`PanOneParamsTest` pins the presets, and the check says in its own comment that
this is a pin and not a proof: it does not show the numbers are right, it makes
changing one a deliberate act that appears in a diff.

And swapping the ids of two destinations survived, which is precisely the
failure the whole file exists to prevent: every id still resolves to an index
that resolves back to the same id, nothing is duplicated or empty, and every
patch anybody saved now modulates the other oscillator. What does change is
that the row reading "Osc 1 pitch" carries the id `osc2pitch`, so the menu and
the project file have stopped agreeing. Where an id carries a number the name
now has to carry the same one, which catches any swap inside a numbered family,
and those are the only swaps a copy and paste produces.

### Mutation testing again: 54 breakages, 53 caught

Four of the original fifty had to be rewritten against the fixed
`anyRouteTo`, and four more were added aimed at the fix itself, including the
shipped fault as a deliberate regression.

| Where | Tried | Caught | Before |
|---|---|---|---|
| `shaped` | 10 | 10 | 10 |
| `evaluate` | 14 | 14 | 11 |
| `apply` | 6 | 6 | 5 |
| the id lookups | 7 | 7 | 6 |
| `Patch`, `anyRouteTo` included | 12 | 11 | 4 of 8 |
| the tables | 5 | 5 | 2 |

### The survivors, and both are genuine no-ops

**`Patch::clear` clearing only the rows in use rather than all of them.** The
rows above `count` keep whatever they held, and nothing can read them: both
readers stop at the count, and `add` overwrites a row before it is used.
`clear()` also had no caller anywhere in `Source`, so it is unreachable twice
over. Not a gap, and not worth a check: a test for it would be asserting
something about memory nothing looks at.

**The `destination < 0` half of `anyRouteTo`'s guard.** Remove it and nothing
anywhere notices, because `isWired()` requires a row's destination to be at
least zero, so no row can ever equal a negative argument however the generated
patches are built. Genuinely unreachable, and it stays because the two halves
of one bound belong together.

The null source array check in `evaluate` reads like a third of these and is
not. A count of nothing already refuses every row through the bound on its own
source, so the check looks like dead code, and it is the only thing standing
between a caller whose count and pointer disagree and a null dereference on
the audio thread. There is now a case for exactly that.

### What the auditor found, because this is the part worth keeping

The fix was handed to the auditor before merging, the way CLAUDE.md says
anything touching the audio thread has to be. It could not break the fix:
`enabled` is the right direction, nothing depends on the old answer,
`PanOneBreed` and `PanOneParams` already agreed with the new one, and nothing
on the audio thread was made unsafe. What it did break was four of the checks
written to close the gaps, and the shape is the same one in all four:

**A new check aimed at a bound, placed where it cannot reach that bound.**
`theBoundsAreTriedAtTheirEdge` had a row with both ends one past the end at
once. The tests run in order, so the row was refused on its destination and
the source bound was never reached: an off by one there is a heap overflow and
it survived the whole suite. One end on its bound at a time, with the other
well inside, kills it.

**The new guard tried fifty past its edge and never on it.** The row with a
source that does not exist used `numSources() + 50`. An off by one admits
exactly one index, so two mutations of the one line the fix adds survived,
including the one that counts destinations instead of sources, which is the
shipped fault resurrected by a different route.

**The stale row placed nineteen rows beyond the bound it pins.** Written at
`routes[20]` with a count of one, so walking one row too far reads
`routes[1]` and finds nothing. Written at `routes[count]` it dies.

**A bit for bit comparison between two patches differing in two things.** The
switched off render also set envelope 4, and matched only because nothing
reads an envelope no live row points at. It compares against the same patch
with its matrix emptied now, which is what the comment always claimed.

Which is the same lesson as the fuzz width, three more times: it is not enough
to aim a check at a bound, it has to be placed where the bound is the only
thing that can refuse it. A check one index or one row away from what it is
testing passes for the wrong reason, and reads in the diff exactly like one
that works.

It also caught a claim in this entry that did not reproduce, now corrected
above, and it was right that the figures through the voice came from a harness
that was not in the repository. They are `Tests/ModMatrixGateProbe.cpp` now.

### Mutation testing, final: 61 breakages, 59 caught

The seven the auditor wrote are included. The two survivors are the two
no-ops above.

### Re-run from a cold start, which found two more of the same shape

The audit above was run against the strengthened file a second time, from a
separately written set of 48 breakages, by somebody who had the mutation
results and the diff but not the session that produced them. Five survived
rather than two, and the three extra are:

**A via on the edge of a short array.** `evaluate` bounds a via twice, once
against the source table and once against the length of the array the caller
handed over, and the existing case put a via one past the end of the **whole**
table. There the table bound refuses the row whatever the array bound says,
so an off by one in the array bound reads as covered and is a heap overflow on
the audio thread. A short array is the only place the array bound is the one
doing the refusing, and the short array case had a row on its source bound and
one on its destination bound and none on its via. Closed, and the check also
pins what an unresolvable via does, which is to be ignored rather than to
scale its row to nothing: the row stands at full depth.

This is the auditor's own finding from this entry for the fourth time, in the
one column it had not been applied to. A bound checked twice needs a case
where each check is the only one that can refuse the row, not one case for the
pair.

**The null destination array check had no case, where the source one did.**
The two are mirrors, and the argument written above for the source check, that
a count of nothing already refuses every row so only a disagreeing count and
pointer reach it, is word for word the argument for the destination check.
One got a case and the other did not. Closed with the mirror of it.

**`rows = patch.count < maxRoutes ? patch.count : maxRoutes` with `<=` for
`<`.** A third no-op, and a cleaner one than either above: both forms are
`min` of the same two numbers, so no input distinguishes them.

Nothing in the fix itself failed the second pass, and the two no-ops it
recorded were confirmed independently: `Patch::clear` still has no caller
anywhere in `Source`, and `isWired()` still makes `anyRouteTo`'s negative
guard unreachable.

### Mutation testing, truly final: 63 breakages, 60 caught

The three survivors are the two no-ops the first pass recorded plus the `min`
one above. Every bound in the file now has a case where that bound is the only
thing refusing the row.

---

## 2026-11-20: the transform every spectral measurement is taken through

### What is being looked at

`Source/RealFFT.h`, which has never appeared in this log. It is picked for
leverage rather than for suspicion.

It is JUCE free, it is 184 lines of pure arithmetic, and it is used in two
different ways that both matter:

- In the studio, `Synth/WaveBank.h` takes the spectrum of every waveform it
  defines as a function of phase by transforming it, and `Synth/WaveTable.h`
  builds every mipmap level of every wavetable by inverse transforming a
  spectrum. So every band limited wavetable the instrument ships with comes
  out of this file. `Spectrogram.h` runs its analysis and its resynthesis
  through it too.
- In `Tests/`, it is the measuring instrument. Nine test files take a
  spectrum through it: `WaveBankTest`, `WaveTableTest`, `SaturatorTest`,
  `TiltTest`, `DitherTest`, `GainRampTest`, `ClipFadeTest` and two probes.
  Several of the measured figures quoted in CLAUDE.md are readings taken
  through this transform.

That second use is the reason to audit it ahead of something more obviously
dangerous. A fault here would not read as a fault: it would move the
wavetables and move the measurements of the wavetables in the same direction
at the same time, and the checks would still pass. It is the exact shape this
project keeps meeting, a number that is present, plausible and wrong, sitting
under the instrument that reads the numbers.

### Mutation testing: 44 breakages, 38 caught

Each one compiled with the sanitizers exactly as `Tests/run.sh` has them, and
the harness verified that the unmutated header passes first.

The forward path is covered about as well as a transform can be, and the
reason is one check: `agreesWithTheDefinition` computes the same answer from
the defining sum and compares. Every one of the 16 breakages of `transform`
and `forward` died, including three that died on a sanitizer rather than on a
comparison, which is the reminder that a plain `-O2` mutation harness would
have reported them as survivors:

| Where | Tried | Caught |
|---|---|---|
| `transform`, the bit reversal and the butterflies | 15 | 15 |
| `forward` | 5 | 5 |
| `setSize`, the twiddle table and the scratch | 7 | 7 |
| `setSize`, the refusal of an impossible size | 4 | 3 |
| `inverse` | 13 | 8 |

All six survivors are in `inverse` or in `setSize`'s refusal, and they split
three and three.

### Three survivors that are real gaps

**`inverse` was never run on a transform whose scratch was cold.** Making the
mirror loop start at 2 instead of 1, so the lowest mirrored bin is never
written, survived the whole file. So did stopping it one bin early. The reason
is that the only check that exercised `inverse` at all was the round trip, and
a round trip calls `forward` first, which leaves the complete spectrum sitting
in the very `scratch` buffer the rebuild is supposed to fill. A bin the
rebuild skips is already correct from the call before it, so the test cannot
see the omission.

That is not an academic gap, because the production path is the other one.
`Synth/WaveTable.h` builds every mipmap level of every wavetable by handing
`inverse` a spectrum it assembled itself, harmonic by harmonic, and never
calls `forward` at all.

Measured rather than argued, because the first version of this paragraph
reasoned that the skipped bin would carry the previous frame's value and that
is not what happens. A table built the way `WaveTable` builds one, three
frames through one transform, asking for a fundamental of 1.0: the shipped
header gives 1.0000, 1.0000, 1.0000, and under the skip-bin-one breakage it
gives 0.0000, 0.0000, 0.0001. The fundamental is simply absent. What sits in
that slot after the first call is not the previous frame's spectrum but
whatever that call's butterflies left there, which measures as nothing in
particular.

So every wavetable the instrument ships with would have been built with no
fundamental at all, which is not subtle: it is the bottom missing from every
patch. And the suite stayed green, because the only check that reached
`inverse` reached it through a `forward` that had already written the answer
into the buffer.

**A refused size leaves a transform that was already usable running at the old
length.** The guard in `setSize` sets `n = 0` before returning. Removing that
one line, so a rejected size merely returns, survived: the existing check only
ever constructs a fresh object, where `n` is already 0 and nothing had to
clear it. Reuse of one transform across sizes is exactly what
`Spectrogram::prepare` does, and it decides whether the size was accepted by
comparing `fft.getSize()` against what it asked for.

### Three survivors that are genuine no-ops, and one of them is a finding

Keeping the imaginary part of the direct current bin, keeping the imaginary
part of the Nyquist bin, and letting the mirror loop run through Nyquist all
survived, and all three really do nothing. Verified rather than reasoned: the
shipped rebuild and each variant were run over 200 random spectra at sizes 2,
4, 8, 64 and 256, with imaginary parts deliberately placed on both of the bins
that cannot have one, and the outputs agreed to the last bit. The mirror case
is an ordering accident, since the write at Nyquist is overwritten immediately
afterwards.

The two bins are the finding. `theOutputIsRealWhateverTheBinsSay` was written
to assert that an imaginary part on either of them is dropped rather than
leaking through as a phantom, and it cannot fail in that direction, because
that is not what delivers the guarantee. `inverse` takes `.real()` of every
sample on the way out, and a purely imaginary component on direct current or
on Nyquist contributes nothing to the real part of any sample: both have real
basis functions, 1 and (-1) to the i. So the output was always going to be the
nearest real spectrum whatever those two lines did. The header's comment
credits the two assignments with the result and is the same shape of fault
this project keeps finding in its own comments: not wrong about the behaviour,
wrong about the mechanism, which is how the next person deletes the line that
is load bearing and keeps the one that is not.

### Also uncovered, found while reading for the mutations

The production sizes were outside the tested range in both directions. The
comparison against the defining sum stopped at 1024 and the round trip at
2048, while `inverse` runs at 512 up to 8192 for every wavetable level and
`forward` runs at 16384 for every waveform `Synth/WaveBank.h` defines as a
function of phase. Neither production extreme was exercised at all.

### What was written

- **`theInverseAgreesWithTheDefinition`.** The inverse compared against
  additive synthesis: the real inverse transform written out as its sum of
  cosines and sines, in terms of only the bins a caller supplies, so the
  reference has no mirroring step of its own to agree with the code about. A
  reference that rebuilt the upper half the same way `inverse` does would agree
  with it about the one thing most likely to be wrong. Run at seven sizes from
  2 to 1024 and from three callers: a transform used for nothing else, one that
  has just run a `forward`, and one that has just run another `inverse`, which
  is what `WaveTable` does on every frame after the first.
- **`oneHarmonicComesBackAtTheAmplitudeItAsksFor`.** The convention
  `Synth/WaveTable.h` builds every wavetable on, that a harmonic of amplitude
  A at bin h is a bin of A * n / 2, asserted against the transform that owns it
  rather than only against the table that comes out of it. At two sizes, four
  harmonics and three phases.
- **`theProductionSizesAreCovered`.** 512 to 16384, which is where the studio
  actually runs and where this file used to stop at 2048. Four partials with a
  known closed form, checked for amplitude, phase and what spilled anywhere
  else. Against a closed form rather than the direct sum because the direct sum
  at 16384 is a hundred million trigonometric calls, and a handful of partials
  still drives every pass of butterflies at the real length.
- **`aRefusedSizeClearsATransformThatWorked`.** A usable transform handed six
  impossible sizes in turn, each time checked for a cleared size and bin count,
  called into to prove it is safe, and set up again.
- **`oneTransformReusedAcrossSizesMatchesAFreshOne`.** `Spectrogram::prepare`'s
  arrangement: one transform set to seven lengths in an order that grows,
  shrinks and grows again, required to agree bit for bit with a fresh
  transform of the same length, forward and inverse.

`theOutputIsRealWhateverTheBinsSay` is kept and its comment now says what it
can and cannot see, since two of the survivors proved it is not a check on the
lines it reads like a check on.

The whole file still builds and runs in a little over three seconds with the
sanitizers on.

### Mutation testing again, against the stronger file

The same 44 breakages: **41 caught, 3 survived**, and the three are the three
that were verified to do nothing. Both of the skipped-bin breakages and the
one that left a refused size in place now die on a comparison rather than on a
sanitizer, which is the right way round for arithmetic.

### One comment corrected rather than made true

The doc comment on `inverse` credited the two assignments that drop the
imaginary part of direct current and Nyquist with making the output real. They
do not, `.real()` on the way out does, and the comment now says so and says why
the distinction is worth the paragraph: a reader who believes those two lines
are the guarantee, and who is later told that the rebuild already makes the
spectrum Hermitian so taking the real part is redundant, deletes the line that
is load bearing and keeps the two that are not.

### What is now proven that was not

- The inverse transform is correct against an independent closed form, from a
  cold transform and from a dirty one, which is the arrangement every wavetable
  in the instrument is built in and the one arrangement nothing checked.
- The transform is correct at the lengths the studio runs it at, rather than
  only up to an eighth of the longest.
- The scaling convention `WaveTable` depends on is pinned in the file that
  owns it.
- A size that cannot work leaves no usable transform behind, on reuse as well
  as on construction.

### Nothing in it was wrong

No fault was found in `RealFFT.h`. The forward path was already covered about
as well as a transform can be, by the one check that compares against the
defining sum, and the arithmetic is right at every size tried. What was wrong
was the coverage of the inverse and one comment's account of its own
mechanism.

---

## 2026-11-19: the voice pool, and the panic that was only as fast as the patch

**Target.** `Source/VoiceAllocator.h`, its test, and the one caller that reads
it, `PanOne::handle`, which led into `AudioEngine::scheduleMidi`.

Picked because a mechanical mutation sweep of the allocator left survivors, and
because reading the caller in order to judge those survivors found two
mechanisms wired to nothing: the steal report the allocator computes and
documents, which `PanOne` assigns to a variable and never reads, and
`Synth::Voice::kill()`, which carries a comment saying what it is for and had
no caller anywhere in `Source`.

**Two real faults were found and fixed. Both are audible and neither announces
itself.**

### Fault one: a chord held across a loop point was killed at the loop point

`AudioEngine::scheduleMidi` silenced everything whenever the play head stopped,
jumped **or wrapped**. `sendAllOff` was `jumped || wrapped || (wasRunning && !
isRunning)`, and `wrapped` is set every time the loop comes round, and also
when an arrangement with no loop range plays past its last bar, because
`loopTo` falls back to the song end.

What it cost, in the order somebody would notice:

- `stopTrackedNotes (slot, true, true)` passes `includeLive = true`, so a chord
  held on a controller while a looping arrangement plays was handed note offs
  at the loop point and nothing brought it back. Playing along with a loop is
  the ordinary way to use one.
- The MIDI recorder reads the live input rather than what the engine schedules,
  so a pad held across the wrap was cut in the monitoring and ran straight
  through the wrap in the take. What was heard and what was recorded disagreed,
  which is the thing this studio is least willing to be wrong about.
- Controller 64 went out at every wrap and nothing anywhere tracks the pedal's
  real position, so holding the pedal down through a loop point turned sustain
  off for the rest of that pass.

**The part worth keeping is that this needed no specification to find.**
`audioDeviceIOCallbackWithContext` already says, in two comments 33 lines apart
and about this same wrap, that a wrap is a continuation: it is why the delay
compensation buffers are not dropped there, and why the gain ramps are not
stepped there. Both guard on `jumped` alone. The MIDI half of the same callback
folded `wrapped` in with `jumped` and contradicted both of them. A comment that
says the opposite of the code, in the same function as the code.

### Fault two: the two channel mode messages were one branch

`PanOne::handle` had `else if (message.isAllNotesOff() ||
message.isAllSoundOff())` and gave both the same gentle answer, releasing every
voice. The MIDI 1.0 Detailed Specification, version 4.2.1, MMA document 96.1-4,
gives them different behaviour, and the reason there are two messages at all is
the difference:

- All Notes Off "should force voices to go to the release stage of the
  envelope, and not terminate the sound of the notes abruptly" (Appendix, "Hold
  Pedal and 'All Notes Off'", p. A-5). The code already did this and it was
  right.
- All Sound Off turns off all notes currently on and sets "their volume
  envelopes to zero as soon as possible" ("Channel Mode Messages", p. 25). The
  code did not do this. Controller 120 was added to the specification after
  controller 123 precisely because All Notes Off is not allowed to be abrupt.

So the studio's panic was only ever as fast as the patch. `AudioEngine` sends
the pedal up, then All Notes Off, then All Sound Off, which is the right
sequence; the last message in it did nothing. On a pad with a four second
release, asking for silence gave four seconds of pad.

And the escalation it was supposed to feed had been hollowed out from the other
end. Pressing Stop a second time is documented in `MainComponent` as the panic
button, for plugins that drone on regardless of note offs. The first press
already sent the harshest message there is, so the second press had nothing
left to escalate to.

Worth noting what was **not** true, because the first version of this writeup
said it and an audit took it out. "A hosted VST3 instrument is chopped dead at
every loop wrap" is too strong. JUCE 9.0.3's VST3 host turns a controller
message into a parameter change or drops it, VST3 has no All Notes Off event,
and JUCE built plugins on `juce::Synthesiser` make the same conflation this
code did, so they are gentle too. The instruments that really would have been
cut are Audio Units and LV2, where the bytes are forwarded. The fault at the
wrap is in the note offs, which reach everything, not in the channel mode
messages, which may reach nothing. A plausible number in the argument rather
than in the signal, which is the shape this file keeps recording.

### What was changed

`Source/TransportSilence.h` is new and is the whole of the decision: four
booleans in, five messages out, no JUCE. It is a header of its own for the
reason `LatencyGraph.h` and `RecordAlign.h` are. The decision lived in the
device callback where nothing could reach it, and three faults were living in
it undisturbed, which is the same post mortem CLAUDE.md already records about
`finishAudioRecording`: the figure was in the window, where nothing tests it.

The table: a wrap gets note offs for the clip notes and nothing else, because a
clip note whose off beat lies past the loop end never reaches its off and would
hang, and everything else sounding is a continuation. A stop or a jump gets the
pedal up, note offs for everything tracked, and All Notes Off behind them for
notes the engine is not tracking, which is what a hosted arpeggiator may be
holding by itself. Only a panic gets All Sound Off.

`PanOne` now gives the two messages the two answers, and All Sound Off reaches
for `Synth::Voice::kill()`, which is what it was written for.
`VoiceAllocator::allSoundOff` is the other half of that and is not optional:
silencing the voices without telling the pool leaves it believing sixteen
silent voices are still sounding, so the next chord steals rather than finding
them free, and the symptom turns up on the chord after the one that caused it.

### Mutation testing

All figures from the same harness, so they can be compared. Mutants that do not
compile are not counted.

| | survivors | of |
|---|---|---|
| `VoiceAllocator.h`, as it stood, with the suite's flags as they stood | 14 | 57 |
| the same, with `-D_GLIBCXX_ASSERTIONS` added and nothing else changed | 11 | 57 |
| the same, with tonight's tests | 10 | 57 |
| `TransportSilence.h` with its new test | 0 | 33 |

**Three of those survivors were not survivors.** `-D_GLIBCXX_ASSERTIONS` makes
the subscript on `std::array` a checked one, and the sanitizers cannot do this:
an array that is a member of an object lives inside that object's own
allocation, so reading one past the end of it reads the next member and address
sanitizer has nothing to complain about. Writing past the end of the whole
object it does catch, and that difference is what decides which of these hide.

The three the flag caught, named exactly, because the first version of this
entry got them wrong and an audit measured them:

- the retrigger loop in `noteOn`, `i < NumVoices` to `i <= NumVoices`
- the same off by one in `findOldest`
- `index >= NumVoices` to `index > NumVoices`, the bounds guard in
  `voiceFinished`

The same off by one in `findSilent` is **not** among them: it was already
caught, without the flag, against the old test. That is not luck and it is the
useful part. `findSilent` returning `NumVoices` makes `noteOn` **write**
`voices[NumVoices]`, which is past the end of the whole object and which
address sanitizer sees. `findOldest` and the retrigger loop only **read**
`voices[NumVoices]`, which lands inside the `counter` member sitting after the
array and is invisible. So the same mistake in three loops hides in two of them
and not the third, which is a poor thing to be relying on.

The flag is now in `Tests/run.sh` and all fifty one tests pass with it.

This is the second time this project has been caught by exactly this. CLAUDE.md
already records four deliberate breakages of bounds checks that looked like
survivors and were out of bounds reads the suite caught. Those were in buffers
the sanitizer can see. A member array is not, and the note in CLAUDE.md was not
enough on its own to find that out.

**One survivor was a real gap in the test, and it was the steal policy itself.**
Changing `findOldest (true)` to `findOldest (false)` at the first fallback
collapses the search to a single pass over everything sounding. The documented
policy prefers a voice that is already fading over one the player is holding
down, whatever their ages; the mutant takes the oldest sounding voice of any
kind. `theOldestReleasingVoiceGoesFirst` released the two **oldest** voices, so
the longest releasing voice and the oldest voice of any kind were the same
voice and the two halves of the policy could not be told apart.
`aFadingVoiceGoesBeforeAHeldOneHoweverYoungItIs` releases the **newest** voice
instead, which is the case where the two answers differ. The audible difference
is a note the player is still holding being cut while something was already
fading away.

**The ten that remain are genuine no-ops.** Said individually, because "no-op"
said in bulk is how a gap hides:

- `none = -1` to `-2`. The value is never exposed as a number. Every reader
  either compares it against `none` itself or tests it for being negative, and
  it is only ever returned at all when the pool has no voices.
- `Voice::startedAt`'s default, to 1 or to minus one. Only read on a voice that
  is sounding, and every voice that starts sounding overwrites it from the
  counter. `findOldest` skips the rest.
- `counter`'s initial value in `reset` and in the member initialiser, to 1 or to
  minus one. Only the order of the numbers matters, never the numbers. Minus
  one on an unsigned counter wraps to zero on the first note and counts up from
  there, so the order survives even that.
- `oldest`'s initial value in `findOldest`, to 1 or to minus one. Dead: `best ==
  none` short circuits on the first qualifying voice and sets both.
- `v.startedAt < oldest` to `<=`. Two sounding voices can never share an age, so
  the tie this changes cannot arise. That was an assumption when the sweep found
  it and it is an assertion now: `invariantsHold` checks that no two sounding
  voices share a `startedAt`, after every one of the fifty thousand random
  events. A mutation sweep cannot tell a tie that is impossible from one that is
  untested, so the invariant is what makes the judgement checkable rather than
  argued.

### The flag found one more thing, in a test nobody was looking at

`main` was already failing CI when this branch started, on `TunerTest`, and
adding `-D_GLIBCXX_ASSERTIONS` made it fail nearly every time rather than
sometimes. That is worth separating out, because the flag is not the fault.

The tuner's tear check runs a writer thread against a reader and, at a capacity
meeting the stated requirement, asserts that at least one snapshot is accepted.
Without that, the check above it is satisfied by a bound that refuses
everything, which is a tuner that never shows a note. The loop ran until the
**writer** had written a fixed amount, and the comment above it said that this
"makes this slower rather than flaky" on a machine that schedules the two
threads unevenly.

It does the opposite, and the comment is the thing that was wrong. The writer
is the thread nothing ever slows down. Anything that slows the reader, a loaded
runner with two cores, a sanitizer, a bounds check inside the copy, lets the
writer spend the whole budget while the reader accepts nothing, and the check
fails against a ring that is working perfectly. Measured here, the accepted
count went 517, then 2, then 19 across three runs of the same binary, and zero
on the runner.

Bounded by the reader instead, it runs until 32 snapshots have been accepted,
with a cap on attempts so that a writer which never starts fails the check
rather than hanging the suite. 621 and 1144 attempts on two runs, against a cap
of fifty million.

The general form is worth more than the fix. **A threaded check whose budget is
spent by the thread it is not measuring is a check about the machine.** It
passed on fast hardware for a day and was red on the runner the whole time,
which is also why a red test on `main` should not be left to stand: this one
had been failing since the morning and the next thing to go red would have had
nothing to stand out against.

### What is proven, and what is still only reasoned

The table is proven. Beyond the 33 mechanical mutants, an audit hand built five
semantic mutations the sweep cannot generate, including the one that caused the
original fault, putting the wrap branch ahead of the stop branch so the flags
accumulate again. All five were caught.

**The wiring is not proven, and nothing in `Tests/` can see it**, because
nothing there compiles `AudioEngine.cpp`. Named breakages that leave all fifty
one tests green, so that this is a known gap rather than a vague one:

- swapping the two arguments of `stopTrackedNotes (slot, silence.liveNoteOffs,
  silence.clipNoteOffs)`, which on a wrap sends live note offs and no clip note
  offs: exactly the fault this night removed, put back
- deleting the `if (silence.allSoundOff)` line, which hollows the escalation out
  again
- `&&` for `||` in the guard above the channel mode loop, after which a stop
  sends no channel mode message at all

This is the same gap the fader law night recorded as "nothing executes the
migration's wiring", and the remedy that worked there was a small harness
stubbing the handful of JUCE calls it needed. Here that is
`juce::MidiBuffer::addEvent` and the three `MidiMessage` factories, and
`Tests/juce-stub` is already the precedent. Left for a later night rather than
done badly at the end of this one.

### Not fixed, and recorded in BACKLOG.md

Four things, all found by auditing this work and all deliberately left:

- **A transport edge lost to a failed try lock.** The edges are computed
  outside the graph lock, so a stop landing in the same block as a plugin being
  loaded can leave a clip note sounding until somebody finds the panic button.
  `panicRequest` is consumed inside the lock and is the shape the others want.
  Concurrency rather than policy, and worth reviewing on its own.
- **The sustain pedal is not modelled at all.** It is why neither answer at the
  loop wrap is right, and why the symptom moved rather than went away.
- **`unloadInstrument` reaches for the engine's panic**, which now cuts rather
  than releases, so unloading a plugin on one channel silences a sounding note
  on another. New as of tonight, and the fix is in `MainComponent.cpp`, which an
  open pull request is already changing.
- **Nothing fades a stolen voice, and now nothing fades a killed one either.**
  Measured, `Voice::kill()` steps up to 0.234 of full scale at the voice output.
  The allocator has computed and documented `stolenFrom` for exactly this since
  it was written and `PanOne::handle` has never read it, so a steal already cuts
  without a ramp. The steal is the common case and the panic is the rare one, so
  the ramp belongs with the steal, which is a change of its own.

## 2026-11-14: the swing half of the groove, which is a number other studios own

### What is being looked at

`Source/Groove.h`, 351 lines, and `Tests/GrooveTest.cpp`, 414 lines, which
covers it. A priority 2 target: a test suspected of being weaker than it looks.

Picked for three reasons. It is now the thinnest cover of any arithmetic
header here that no night has audited and no open pull request is changing:
351 lines of source to 414 of test, where the ratio elsewhere runs from two to
four. `Groove.h` grew by 264 lines in the feel merge and `Tests/GrooveTest.cpp`
was not touched in that merge at all, so whatever it was missing before, it is
missing over a larger header now. And a swing percentage is a claim about
agreement with every other studio that reports one, which is the shape of
number this project has been wrong about before.

The feel half, `Source/GrooveTemplate.h` and `Tests/GrooveTemplateTest.cpp`,
was mutation tested in its own merge and is not the target. Nothing in
`Source/LoudnessMeter.h`, `Source/TruePeak.h`, `Source/Project.h`,
`Source/Punch.h`, `Source/RecordAlign.h`, `Source/Meter.h`,
`Source/SongLength.h`, `Source/NoteSelection.h`, `Source/LatencyGraph.h`,
`Source/Spectrogram.h`, `Source/Fx/BellMatch.h`, `Source/Envelope.h` or
`Source/Fx/Svf.h` is touched here, because open pull requests are changing all
of them. `Source/Fx/Unit.h` is still the only JUCE-free header in `Source` with
no test, and still nothing includes it, so it is still not worth one.

### The test was weaker than it looked

48 mutations of `Groove.h`, every one built the way `Tests/run.sh` builds
(`-fsanitize=address,undefined -fno-sanitize-recover=undefined`) and judged
against both tests that include the header, `GrooveTest` and
`GrooveTemplateTest`. **28 caught, 20 survived** against the test as it stood
at the start of the night.

Where the survivors were concentrated says more than the count. The swing
positions themselves are watertight: all seven mutations of the shift, all
three of the grid window and all three step lengths were caught, most of them
by two tests independently, which is what a check worked out from the
definition rather than from the code buys. Everything that survived was in
one of three places nothing was asking a question about.

**The humanising draw.** Five of them. The test asked only two things of it:
the same answer twice, and never more than a quarter step. A draw that was
always late, half the size it claims, the same number for every step, the
same number for every note on a step, or a steady ramp rather than a scatter
satisfied both. The first of those is the one worth naming: with the draw made
unipolar the humanise knob becomes a delay of random length, every note in the
pattern late and none early, which against a loop is heard as the part
dragging rather than loosening.

**The velocity half of `shade`.** Five. The off grid window could be deleted
outright and the suite passed, so a part played by hand would have had every
other sixteenth of it softened, which flattens exactly the dynamics somebody
played. The softening constant could be doubled or divided by ten with nothing
noticing, and the ceiling clamp had no case at all.

**The swings the interface offers.** Five. The index clamp in `offeredSwing`
could be removed, which turns those four lines into an out of bounds read; the
shuffle entry could be rounded from a third to 0.66, which lands the offbeat
0.7 ms late at 120 bpm and still passes the round trip through
`nearestOfferedSwing` that was the only check on the table; and the tie rule in
`nearestOfferedSwing` could be relaxed, which decides whether a stored swing
of 0.52 reads as straight and therefore whether the groove can be switched
off.

Also untested: the whole early half of what a project may contain. `ProjectIO`
admits a swing from 0.3 to 0.8 and this file swept 0.5 to 0.75, so a push,
where the offbeat plays ahead of its halfway point rather than behind it, was
never placed at all.

### The fault

**A swing a project file may carry played a pattern's own notes in the wrong
order.**

The feel half bounds its deviation and the humanising draw together, for
exactly this reason, and the swing half did not. The argument written in the
header was that at the shuffle the interface offers, the shift is a third of a
step and the draw fits in what is left, so a pair can meet and not cross. That
argument is about what the interface offers. `ProjectIO` admits a swing from
0.3 to 0.8, and above 0.75 the shift alone leaves the draw nothing to spend:
the crossing is `2 * swing - 1.5` steps.

Measured over a hundred thousand placements at 0.8 with the humanise knob up,
a note landed up to **0.096 of a step** behind the note in front of it, which
at a sixteenth and 120 bpm is 12 ms. The predicted figures and the measured
ones agree across the band: 0.0176 against 0.02 at swing 0.76, 0.0535 against
0.06 at 0.78, 0.0964 against 0.1 at 0.8.

The shift and the draw are now bounded together at three quarters of a step,
which is what the neighbours leave once their own humanising is counted: the
downbeat behind may be drawn a quarter of a step late and the one in front a
quarter early, so a note may travel three quarters of a step and meet one of
them without passing it. The bound binds only above 0.75, so nothing the
controls can ask for is changed by it, and with the humanise knob down the
swing still lands exactly where its number says whatever the file carries,
which the test now asserts to 0.8.

How reachable: not from the interface, which offers nothing above a triplet
shuffle, so this needs a hand edited `.ahp` or a file written by a build whose
offered list reached further. It is in the band `ProjectIO` deliberately
admits, and the previous night recorded the crossing in the backlog as
something a later night should decide about rather than as a fault. It is a
fault: notes playing out of order is not a degree of swing. The backlog entry
saying it is open is now stale, and is left alone rather than tidied because
two open pull requests are changing that file.

Two comments also said the opposite of the code, which is the shape this
project keeps meeting:

- `Groove.h` said a stored swing of 0.6 "can only be shown as Straight". It is
  shown as 0.58, and `GrooveTemplateTest` has asserted that it is not shown as
  Straight since the day it was written, so the comment and the suite
  contradicted each other in the same repository.
- `GrooveTemplateTest` said the guard in front of `llround` is a guard against
  something unspecified rather than against an observable fault. That is true
  of the huge finite positions and not of a NaN, which compares false against
  the on grid test and so goes through: with the guard removed such a note is
  placed at the start of the clip rather than left where it is. `GrooveTest`
  now pins that.

### The audit of the fix, which found seven things around it

The clamp was audited by a reader who had not written it, which is what
`.claude/agents/auditor.md` is for, and the headline is that the bound is
right: exhaustively, at a thirty-second, swing 0.8, amount 1, humanise 1, over
every one of the 128 note numbers on both sides of every join, the worst gap
between a step and the next is `+2.7e-7` of a step. Positive, so nothing
changes places, and essentially zero, so the bound is tight rather than
generous. Removing it reproduces the fault exactly, and the closed form
`2 * swing - 1.5` is confirmed to the digit at 0.76, 0.78 and 0.80. A feel
deviation cannot be added on top of a swing, because `place` branches on
`byFeel`, so the two bounds never compose.

**The bound was sufficient but not self sufficient, which is the same mistake
one field over.** The offbeat's total was held to three quarters of a step.
The downbeat beside it was held to a quarter only because `ProjectIO` clamps
the humanise amount to one: nothing in `Groove.h` said so, and the whole
argument for the clamp is that a file carries numbers the interface will not
offer. At a humanise of 1.2 and a swing of 0.8 the unclamped downbeat put the
crossing back, half of it, 0.05 of a step. Not reachable from a file today, and
not claimed as a live fault: it is the guarantee resting on another file's
bound, which is exactly the shape the fix exists to remove. The downbeat's
quarter is now clamped too, so the pair divides exactly one step whatever a
file carries, and the ordering sweep asks for humanise amounts past the one the
loader admits.

That sweep also had to learn the difference between two promises. Within the
band a file can carry, every note is strictly after the one before it, because
`|jitter| < 1` strictly and neither bound is quite reached. Past that band both
bounds saturate, and since the pair divides exactly one step, saturating both
puts two notes on the same beat. That is not a reordering, and it is what the
feel half's own comment already calls allowed, so what is asked out there is
that the order never reverses. Asking for the strict version everywhere would
be asking the budget to be less than the step it divides.

**How much of the humanising the bound costs was a number nothing asked
about.** A bound of 0.70, 0.65 or 0.60 all passed the whole suite. At 0.60 and
a swing of 0.8 every offbeat's draw is cut off at exactly the swung position,
so more than half of them pile onto one instant and the knob can only pull
early, which is the opposite of humanising. The reach either side is now
pinned at the top of the band: 0.15 of a step late against the full 0.25 early.

**The draw's yardstick claimed more than it had.** The comment said each of its
measurements was pointed at a generator known to fail it first. In fact it had
five terms and four generators, two of which failed on the same term; nothing
exercised the spread at all; and four of the five terms could be deleted
outright with the whole suite still green, so only the correlation test was
load bearing. There is now one generator per term that fails that term and
passes the other four, which is the only arrangement under which deleting a
term is caught, and each is asserted to fail for its own reason rather than
merely to fail. The two lopsided generators take their own mean off, because
reaching one end of the range and not the other is otherwise given away by the
mean first.

Its three resolutions were also printing the identical sequence three times,
since the draw is a fraction of a step and the hash keys on the step index. Two
of the three passes could not fail if the first did not, and a reader comparing
the rows would reasonably have read the agreement as evidence. The invariance
is now asked outright, and a draw keyed on the beat rather than on the step
fails it.

**Three comments said things the code does not do, two of them written the same
night as the fix.** This is the fourth time this file records that shape.

- `Groove.h` said only something within a hundredth of straight reads as
  straight. The entries are a fortieth apart and the tie goes to the straighter,
  so it is two fortieths: anything up to and including 0.52, which the test in
  the same commit asserted. The table is also one sided, so every swing in the
  early half of the band a file may carry reads as straight and is switched off
  when a feel is cleared. That is the intended answer, since the controls cannot
  show a push at all and a groove left in a state no control can reach is the
  worse outcome, but it was nowhere written down.
- `Groove.h` said the humanising had nothing left to spend at a swing of 0.8.
  It has 0.15 of a step late and the whole 0.25 early, with about one offbeat
  in five landing on the bound and an early bias of a hundredth of a step. And
  the 0.096 of a step quoted as the crossing was a hundred thousand draws
  falling short of a bound that has a closed form: it is a tenth of a step.
  A sampling shortfall presented as a bound is the same failure in miniature as
  the true peak meter reading the largest sample.
- `ChannelRackComponent.h` still carried the claim this night set out to
  correct, with the same 0.6 example, directly above the snapping code it
  justifies, so it was the copy the next reader would believe. It also said
  snapping moves a swing by at most a fortieth, which is true inside the range
  the box covers and not outside it: a stored 0.8 snaps to the triplet shuffle,
  0.133 in the number and 0.27 of a step on the offbeat.

Two things the audit raised were deliberately left alone. `Research/BACKLOG.md`
still carries the entry calling the crossing open and offering "clamp
`grooveSwing` to 0.75 on load" as one of two remedies: that one must not now be
done, since it would silently move an existing 0.8 file's offbeats by an eighth
of a step for no remaining reason. The entry is stale and the file is being
changed by an open pull request, so it is recorded here rather than edited
there. And offering a push in the interface, so that the early half of the band
a file may carry becomes representable, is a decision about the interface
rather than about this header.

### Where it stands

**58 mutations, 46 caught, 8 survived**, every one built the way `Tests/run.sh`
builds and judged against both tests that include the header. All 48 test
programs pass.

Three further mutations were discarded rather than counted, and the reason is
worth keeping. Turning a leaf assertion of the test into a tautology
(`othersFailed >= 0`, `draws == draws`, `late >= 0.0`) survives by construction
and measures nothing: a weakened assertion cannot make its own suite fail. What
does show such an assertion is load bearing is breaking the code it judges,
which is where `bound-offbeat-seven-tenths` and
`draw-keyed-on-the-beat-not-the-step` earn their place. Mutating test machinery
is only meaningful where it is shared, as the five `yardstick-term-dropped`
mutations are: dropping a term makes a generator built to fail it be accepted,
and the test says so.

The eight survivors, each with a judgement. All eight were survivors before
this night's work too, and none of them is a gap in a test:

- `jitter-drawn-when-the-knob-is-down`, the draw's guard relaxed from
  `random > 0.0` to `>= 0.0`. **Genuine no-op.** The hash is computed and then
  multiplied by zero. It costs a multiply and two shifts per note of every
  pattern and changes no output.
- `jitter-spent-when-the-knob-is-down`, the same relaxation where the draw is
  spent. **Genuine no-op**, adding exactly 0.0.
- `step-fallthrough-is-an-eighth`, the `return` after the switch in
  `stepBeats` changed. **Genuine no-op.** The switch covers all three
  enumerators and `ProjectIO` clamps the stored base to 0 to 2, so nothing can
  reach it.
- `shade-finite-guard-removed`. **A guard against the unspecified, not a gap.**
  For the huge finite positions the unspecified number `llround` gives back
  then fails the on grid test, which is what the guard does too. For a NaN the
  result depends on whether that unspecified number is odd, and in this build
  it is even, so the velocity comes back untouched either way. A check written
  around that would be pinning the compiler's choice rather than the
  behaviour. `place`'s equivalent guard is observable and is pinned.
- `nearest-starts-from-zero`, the loop in `nearestOfferedSwing` starting at 0
  rather than 1. **Genuine no-op**: it compares entry 0 against itself under a
  strict `<`.
- `nearest-nan-guard-removed`. **Genuine no-op.** Every comparison against a
  NaN is false, so `best` stays at 0, which is what the guard returns. The
  guard says the intent out loud and nothing more.
- `feel-empty-slot-gets-a-feel`, the `filled` test dropped when reading a
  slot's deviation. **Genuine no-op.** An unfilled slot carries a deviation of
  zero by construction, which `GrooveTemplate.h` enforces in one place and
  documents there as defence in depth.
- `place-huge-position-guard-removed`, the `1e15` half of `place`'s guard,
  leaving the `isfinite` half. **A guard against the unspecified, not a gap**,
  and the same judgement as `shade-finite-guard-removed` above for the same
  reason: past that range `llround` gives back an unspecified number, which
  then fails the on grid test, so the note is left alone either way. The NaN
  half of the guard is the observable one and `GrooveTest` pins it.

Nothing else in the header is now unasked. The swing positions are checked
against the definition rather than against the code, in both directions and
across the whole band a file may carry; the draw is measured rather than only
bounded, with the yardstick pointed at four generators built to fail it before
it is believed; `shade` has the same window, the same amount and the same
clamps pinned as `place`; and the table of offered swings is checked for being
straight first, increasing, a real triplet at the top, clamped at both ends and
decided the straighter way on a tie.

---

## 2026-10-29: the loudness meter, which is the one number other people check

### What was looked at

`Source/LoudnessMeter.h`, 409 lines, and `Tests/LoudnessMeterTest.cpp`, which
covers it. A priority 2 target in the brief's ordering: a test suspected of
being weaker than it looks. Nothing had merged since the night of 2026-10-28,
so there was no recently merged work to audit, and the only JUCE-free header in
`Source` still without a test is `Fx/Unit.h`, which two earlier nights set
aside as unreferenced scaffolding and which still has nothing including it.

Picked on the ratio and on the stake. 409 lines of source to 534 of test was
the thinnest cover of any real arithmetic header here. And every other number
this studio reports is checked by the owner's own ears, while a LUFS figure is
checked by Spotify, Apple Music and YouTube, which is the entire reason for
measuring it. A meter half a decibel out is not slightly wrong: it is making a
claim about agreement that it cannot support, and nothing about the number
looks wrong.

Not `Fx/Svf.h` or `Tests/SvfTest.cpp`, which open pull request #55 is changing,
and not `AudioEngine.*`, `InputSource.h`, `MainComponent.*`, `Tuner.h`,
`TunerPanel.h` or `Tests/run.sh`, which open pull request #61 is changing.

### The test was much weaker than it looked, and one thing was wrong

108 mutations of `LoudnessMeter.h` against the test as it stood, every one of
them built the way `Tests/run.sh` builds
(`-fsanitize=address,undefined -fno-sanitize-recover=undefined`). **64 caught,
44 survived.** That is the worst ratio any night here has recorded, and it has
one cause: every single case in the test put the same signal in both channels,
and every case but one measured a steady tone. A steady tone reads the same
through a window of any length, and two identical channels cannot tell a sum of
two channels from one channel counted twice.

The method was the two pass one, fast build first and then the sanitizers on
everything that survived it, and that mattered: `ring-pre-increment` and
`ring-wrap-off-by-one` both write one past the end of the 400 ms window, and
both pass a plain `-O2` build. They are the fourth and fifth breakages on this
project to look like survivors and turn out to be out of bounds accesses, so
the warning in CLAUDE.md and in both agent descriptions is now five for five.

### The fault

**A programme above +29.995 LUFS reported no integrated loudness at all.**

The histogram runs to +30 LUFS, so its top bin is where everything past that is
clamped: a catch all, rather than the hundredth of an LU every other bin is.
The relative gate sits 10 LU under the gated mean, so a programme that loud
puts the gate in that same top bin, and `updateIntegrated`'s rule that decides
the gate's own bin by its centre then discarded the one bin holding every block
there was. `count` fell to zero and the reading fell to the -200 sentinel.

Measured at +45 LUFS: **-200.000 before, +45.007 after.** The fix is one
condition, exempting the top bin from the centre rule, because its centre says
nothing about its contents.

How reachable: it takes about thirty decibels of gain past full scale, so it
takes a mistake upstream, a plugin with a broken output stage or a gain
staging accident. Which is exactly when somebody opens the mix report to find
out what happened. What it told them was the quietest possible number for the
loudest possible signal, which reads as silence. That is the shape this project
keeps meeting, except inverted rather than approximate, which is worse: a
figure 3 dB out invites doubt, and a figure that says silence while the room is
deafening sends the owner looking at their monitors.

Past the top of the histogram the figure degrades rather than being exact, and
the first version of this entry and of the header comment both said exact,
which the auditor caught and which is worth recording as the mistake it was.
The top bin is a catch all, so the relative gate cannot separate the blocks
inside it: everything above +30 is counted whether it belongs above the gate or
below it, and the figure comes out as the ungated mean of all of it. Measured
on 5 s at +60 followed by 50 s at +31, where the gate at +49.5 should drop the
quiet fifty seconds: +49.540 reported against an exact gated figure of +59.875,
so 10.3 LU low. Randomised over four hundred thousand programmes the worst case
is 9.5 LU low. The range collapses to nothing up there for the same reason.

So the fix is better rather than right, which is the honest way to put it, and
it is still the right change: a finite figure that says the master is tens of
decibels past full scale is what somebody needs at that point, and no figure at
all is not a quiet reading but the absence of one. `MixReportPanel` hides the
whole report below -150, so what the owner actually saw was a report that never
appeared and a line inviting them to press play. Whether to raise the ceiling
is a separate decision, written up in `Research/BACKLOG.md` with the
measurement, because any finite histogram has a top and raising it narrows the
band this happens in rather than removing it, at the cost of a longer scan in
the device callback. The test now pins what is true up there rather than what
would be nice: finite, and still above +30.

One other number in the header was wrong in the line this change rewrote. It
said a converter would have clipped forty decibels before +30 LUFS. A 1 kHz
sine at 0 dBFS in both channels reads 0 LUFS, which the test now asserts, so it
is thirty.

### What the survivors asked for, and what was written

Eleven new checks, each one there because a breakage survived everything
already in the file:

- **Both channels, and their sum.** Reading the left channel twice passed every
  compliance case. Now: a tone in the left reads the same as the same tone in
  the right, both channels together read exactly 3.0103 dB above one, a
  different tone in each channel reads the power sum of the two single channel
  readings to five decimal places, and a peak present only in the right
  channel is reported. That last one needed its own case because the peak
  detectors are per channel too.
- **A mono call.** `process` promises to measure at most the first two
  channels, and that promise is the only thing between a mono bus and a read
  past the end of the caller's array. Asserted with an array that has exactly
  one entry in it, so the sanitizers would stop on a second read, and the
  reading has to be 3.0103 dB under the same signal in two channels.
- **The 400 ms window.** A 300 ms one passed, because a steady tone reads the
  same through any window. A tone sounding 100 ms in every 400 ms does not: a
  correct window always holds exactly a quarter of it and reads 6.0206 dB down,
  steadily, at every hop. A 300 ms window holds between none of it and all of
  it depending on where it sits. Measured: -29.014 against the -29.021 the
  duty cycle says, and steady to four decimal places.
- **The 3 s window, from both sides.** Nothing before 3.3 s, which is when the
  thirtieth 400 ms block lands. One second after a 20 dB step the reading is
  still at -28.36, and 3.3 s after it has arrived at -22.99. A shorter window
  fails the first and a longer one the second.
- **Both ends of the histogram.** A -69.5 LUFS programme reads -69.493, a
  -70.5 LUFS one reads nothing at all, and +31 and +45 LUFS programmes read
  themselves. The quiet end also exercises the clamp in `binFor`, which nothing
  did before: the relative gate only falls off the bottom of the histogram
  when the programme is quiet enough for its mean minus 10 LU to sit under -70,
  and the quietest programme anywhere in the old test was -36 dBFS.
- **The curve at every rate a device can open at.** Designing the 38 Hz
  highpass at a fixed 48 kHz passed everything, because the sweep only ever
  visited 48 kHz and the two checks that do visit other rates both use a 1 kHz
  tone, where that filter is flat to a hundredth of a decibel. At 96 kHz its
  corner would sit at 19 Hz instead of 38. Now measured relative to each rate's
  own 1 kHz reading, which is the right way round: the shape is what the
  weighting is, and the absolute calibration is pinned separately because the
  standard's highpass numerator is not normalised, so its passband gain moves
  by two hundredths of a decibel between rates.
- **A peak reading with an upper bound.** The old check only ever asked for
  more than 2.5 dBTP, so measuring the peak after K-weighting passed it
  comfortably: the weighting lifts the 12 kHz signal that case uses by about
  4 dB, so the reading went further up rather than wrong. Everywhere else it
  would read high, which is the direction that tells somebody a master is over
  when it is not. Plain sines at 50, 400 and 1000 Hz now have to read their own
  amplitude within a tenth of a decibel.
- **The loudness range's own gate.** It could be 10 LU instead of 20 and
  nothing noticed, because every passage the old cases gated out was far enough
  down that any threshold between 10 and 30 would have removed it. The gate
  matters in the ordinary direction: a chorus 15 LU above a verse is a dynamic
  record, and a 10 LU gate throws the verse away and reports it as flat. Now a
  passage 15 LU down reads 15.000 LU and one 25 LU down reads 3.450.
- **Reset.** It was not called anywhere in the test, and thirteen separate
  breakages of it survived. It is a live path: the mix report's reset button
  sets a flag the device callback acts on. Now a meter that has measured a loud
  programme, been reset, and measured a staircase reads bit for bit what a new
  meter reads on that staircase, and one second after a reset it reports
  nothing for the three second figure rather than the old programme's.
- **Preparing again.** The owner changing the device's sample rate runs
  `audioDeviceAboutToStart`, which prepares the meter again, and `prepare`
  resizes the 400 ms window. If it did not also reset the position into that
  window, a change from 96 kHz down to 44.1 kHz would leave the position past
  the end of the shorter buffer. Nothing had prepared a meter twice.
- **That none of it allocates.** The header opens by saying everything in it
  runs on the audio thread and the histograms exist for that reason, and
  nothing checked it. The test replaces the global `new` and `delete` and
  requires zero allocations across a second of metering, a reset, and a block
  after it.

A staircase rather than two levels for the reset check, and the reason is worth
keeping: what a reset can get wrong is the relative gate, by carrying the old
programme's block count into the new mean, which moves the gate by a decibel or
two. A programme of two levels does not notice a gate moving if it stays on the
same side of both. A programme spread across the histogram notices at once.
Measured: 0.58 LU of error on the staircase, and none at all on two levels.

Two other things changed. The curve tolerance went from 0.03 dB to 0.00002,
because designing from the prototype reproduces the standard's own 48 kHz table
to **1.6e-07 dB**: the old figure was discarding five orders of magnitude of
resolution and let six different constants be rounded, among them writing `sin`
where `tan` belongs, which costs 0.024 dB. And the header claimed verification
against EBU Tech 3341 cases 1, 2 and 15 to 19. The cases its own test runs are
1, 2, 3 and 9; 15 to 19 are the peak cases and are run against the detector in
`TruePeakTest`. Said accurately now.

### After

109 mutations against the strengthened test, the two that aimed at code since
removed having nothing left to break and one new one aiming at the fix itself.
**94 caught, 15 survived**, against 64 and 44 before. All fifteen are no-ops,
and the judgement on each is below. The shape of them is worth noting: nine of
the fifteen are the histogram's own quantisation, which is the one
approximation the header already says it makes.

- **`binat-no-half`**, a bin's lower edge where its centre belongs: half a bin,
  0.005 LU.
- **`binfor-ceil`** and **`binfor-off-by-one`**, every block one bin up, so
  half a bin of displacement in where the gate falls. No-ops by design rather
  than by luck. The header says the bins hold the
  exact energies rather than their own value, so the only approximation left is
  which side of a gate the one bin containing it falls on. These three are
  that approximation, measured.
- **`integrated-tie-flipped`** and **`lra-tie-flipped`**, the centre rule
  inverted for the gate's own bin: one bin's blocks in or out. Still a no-op,
  but the first version of this entry gave the bin width, 0.01 LU, as though it
  were the error, and it is not. A block crossing the gate moves the reading by
  10*log10((n+1)/n) for the n blocks already counted, which is 1.16 LU on a
  five block programme, and on two equal passages 12.7875 dB apart, where the
  gate lands exactly on the quieter one, measures a **2.75 LU step**. What the
  half bin bounds is the disagreement with another meter, because the standard
  has the same cliff in the same place, and half a bin is a twentieth of the
  tolerance compliance allows. The verdict stands and the reason given for it
  did not.
- **`lra-rank-fraction-total`**, **`lra-rank-no-plus-one`** and
  **`lra-seen-strict`**, each moving a percentile by one rank. Tech 3342 says
  to take the 95th and the 10th percentile and does not say which of the
  several nearest rank conventions to use. libebur128 rounds where this floors,
  which agrees at the 95th and is one rank apart at the 10th, worth a few
  hundredths of an LU on a real distribution. Deliberately not changed:
  swapping one unspecified convention for another is not a fix.
- **`chain-highpass-first`**, the highpass before the shelf: exact. Two linear
  time invariant sections in cascade commute and each carries its own state.
- **`filled-no-clamp`**, `filled` growing without bound: no-op for every
  reading, since it is only ever read as `filled >= blockSamples`. The clamp is
  still load bearing, because `filled` is an `int` stepped once per sample and
  without it a rolling transport overflows it after about twelve and a half
  hours at 48 kHz. Catching that would take a twelve hour test.
- **`hop-off-by-one`**, a hop of 4801 samples rather than 4800: 100.02 ms
  instead of 100. Every figure here is a mean square over a window and a
  hundredth of a per cent of hop moves none of them by anything approaching
  the 0.1 LU the standard allows.
- **`integrated-no-gate-guard`** and **`lra-min-count-gone`**, two early
  returns that never fire: redundant, proven. With nothing gated the mean is
  0.0 over 0, which is a NaN, `loudnessOf` answers -200 to it because NaN
  fails its `> 0.0` test, the gate lands under the histogram, the clamp in
  `binFor` pins the scan to bin 0, nothing is in any bin and the same -200
  comes out of the bottom. Both kept as statements of intent.
- **`lra-no-floor-at-zero`**, the `max (0.0, ...)` on the range: the percentile
  walk is monotonic in its fraction, so the 95th can never come out below the
  10th. Defensive, kept.
- **`reset-keeps-ring`**, `reset` keeping the short term ring's contents and
  its running sum and resetting only the position: a no-op, and a neat one.
  `blocksSeen` is still reset, so nothing is reported until thirty new blocks
  have landed, and by then every stale entry has been subtracted and every new
  one added, so the running sum has arrived at the right value by its own
  bookkeeping. The `fill` and the `blockRingSum = 0.0` are belt and braces.
  Kept, because they cost nothing and the next person should not have to work
  that out.

Two guards were taken back out rather than kept. The fix above needed the top
bin exempting from the centre rule in `updateIntegrated`, and the same rule
appears twice in `updateRange`; exempting it there as well was written, then
mutation tested, then removed, because up there every reading that survives the
gate is in that one bin and no percentile can separate them, so the range is
zero either way. The comment in `updateRange` now says so, to stop the next
reader adding a line that nothing can distinguish from its absence.

### Audited by somebody who did not write it

As CLAUDE.md requires for anything touching audio, and it earned its keep. The
`auditor` agent was given the change and the five things the author was least
sure of. Four held: the fix is sufficient and cannot leave `count` at zero at
either end of the histogram, the bottom bin genuinely needs no exemption, the
two exemptions left out of `updateRange` are no-ops over four hundred thousand
randomised histograms and byte identical output on the whole suite, and the
0.00002 dB tolerance has eight orders of margin rather than one, since nudging
every `std::sin` in the test by an ulp moves the measured error by nothing at
all once it is cast to float.

The fifth did not hold. The claim that the figure stayed exact past the top of
the histogram was false by up to 10 LU, in three places, one of them the
sentence justifying the fix. All three are corrected above. It also found that
the replaced `operator new` leaves AddressSanitizer's heap checking and
LeakSanitizer both intact, proven by planting a bounds error and a leak and
watching each one get reported through the replacement, which is worth knowing
because that check is only worth anything if the hook really does intercept the
meter's own vectors.

And a premise the author had wrong: the suite does not run on three platforms.
`build.yml` runs `Tests/run.sh` in one `ubuntu-latest` job and the platform
matrix never compiles anything under `Tests/`, so Windows and macOS exposure is
a developer running it locally.

### What is now proven that was not

That both channels are measured and that their energies add. The equal weight
itself is pinned by compliance cases 1 and 2 and by the 3.0103 dB check, not by
the power sum case, which takes its expectation from the meter's own single
channel readings and would be satisfied by a meter weighting both channels by a
half. The comment on it says so now, because the other reading of it is
flattering. That a mono call
reads one channel and nothing past it. That the momentary window is 400 ms and
the short term one three seconds, rather than merely some length that a steady
tone cannot tell apart. That the weighting curve is the same curve at 44.1,
88.2, 96 and 192 kHz and not only at the 48 the standard tabulates, to within
0.02 dB. Not below about 32 kHz, where the bilinear warping grows past that:
0.051 dB at 22.05 kHz, still inside what compliance allows, and now said in the
check rather than covered by a claim about every rate. That the curve reproduces the standard's own table to 1.6e-07 dB, so
none of its constants can be rounded. That the peak reading is of the waveform
and not of the weighted signal, from both sides. That the absolute gate is at
-70 LUFS and that the histogram holds whatever it is given at either end. That
the loudness range's relative gate is 20 LU and not 10. That reset leaves the
meter indistinguishable from a new one. That a change of sample rate does not
leave the window indexing past its own end. And that none of it allocates,
which the header has claimed since it was written.

---

## 2026-10-28: the fader law, and the translation of every fader already saved

### What was looked at

Pull request #59 merged at 00:33 UTC today and is the newest thing in the
repository: `Source/FaderLaw.h`, 406 new lines with no JUCE, plus its wiring in
`AudioEngine.{h,cpp}`, `MixerComponent.h`, `ProjectIO.cpp`, `AutomationCurve.h`
and `PlaylistComponent.cpp`. Picked as an audit of recently merged work. The
night of 2026-10-27 set these files aside by name because #59 was open and
changing them, so nothing had read them since they were written.

It is also the highest stake shape of change this project sees. The law being
wrong makes a fader feel odd; the migration being wrong moves every fader in
every project the owner has ever saved, and neither errors, crackles nor looks
wrong on a meter.

### The law and the migration are right, and the test is as strong as it looks

Said plainly because it is the result. 47 mutations of `Source/FaderLaw.h`,
every one built with the sanitizers `Tests/run.sh` uses
(`-fsanitize=address,undefined -fno-sanitize-recover=undefined`). 37 caught,
10 survived, and **every one of the ten is a provable no-op rather than a
gap**, which is unusual and is the reason this entry is short on findings
about the arithmetic.

Caught, among others: every one of the eight segment constants in the forward
law and all four in the inverse, each of the three breakpoints moved, the two
middle segments swapped in both directions, the clamp removed, a corrupt
position reading as unity rather than as the floor, amplitude decibels written
as power decibels in either direction, the ceiling, the floor, the unity point,
the old travel restored, the old law's square dropped, and both of the two
ways to get the migration wrong that the header warns about: routing it
through the fraction of the travel rather than through the gain (caught at
0.640000 against 0.750000) and handing a gain to `dbToPosition` as though it
were a decibel figure.

The ten survivors are three families, and each is a no-op for a structural
reason rather than by luck:

**Six boundary flips, `>=` to `>`, at every breakpoint in both directions.**
The law is continuous at all three breakpoints, so the segment above and the
segment below return the same double there and which one claims the boundary
cannot be observed. Measured at the breakpoints: the two segments differ by
exactly 0.000e+00 at 0.0625, 0.25 and 0.5. The same holds for the inverse's
two end guards: at -90 dB the floor branch gives position 0 and the bottom
segment also gives exactly 0, and at +10 dB the ceiling branch gives 1 and the
top segment also gives exactly 1.

**Three guards whose fall-through reaches the same answer.** Dropping the NaN
check in `gainToPosition` sends a NaN through `log10` into `dbToPosition`,
which rejects a non-finite figure itself and returns the same 0. The header
already says this in as many words, and it is now confirmed by measurement
rather than by reading. `gain <= 0` relaxed to `gain < 0` sends a gain of zero
through `log10` to minus infinity and `dbToPosition` saturates it to the same
0. `legacyPosition <= 0` relaxed to `< 0` reaches `legacyPositionToGain`,
which returns 0 for a position of 0, and `gainToPosition` of 0 is the same 0.
All three guards exist to make the intent readable and to keep a logarithm of
zero off the path, not to change an answer, which is what the header claims
for them.

**One redundant clamp.** Removing `std::min (legacyCurveValue, 1.0)` from
`legacyCurveValueToPosition` changes nothing, because
`legacyPositionToGain` clamps to the old travel itself one call further in. A
curve value of 2.0 gives 0.943820 either way.

### Six claims that nothing produced, one of them load bearing

The arithmetic is sound and the prose around it was not. Every figure below was
checked against the real header rather than reasoned about, and all six are
corrected and now asserted in `FaderLawTest` so they cannot drift again.

**The sentence justifying the one irreversible decision in the change.** The
project file format version is bumped unconditionally at `ProjectIO.cpp:64`, so
a file saved by this build cannot be opened by an earlier one at all. The
paragraph defending that said an older loader would read "+3.5 dB on every
strip at once". It is wrong in size, in sign and in shape. Measured against the
old law preserved in `legacyPositionToGain`:

    stored 1.0000  new law  +10.000 dB   an old build reads  +3.876 dB   -6.124
    stored 0.7500  new law   +0.000 dB   an old build reads  -1.121 dB   -1.121
    stored 0.5000  new law  -10.000 dB   an old build reads  -8.165 dB   +1.835
    stored 0.2500  new law  -30.000 dB   an old build reads -20.206 dB   +9.794
    stored 0.0625  new law  -60.000 dB   an old build reads -44.288 dB  +15.712

A mix left at unity comes back 1.12 dB **quieter**, not 3.5 dB louder, and the
error is a different number at every point on the travel: a spread of 21.84 dB
across it. So the failure is a balance change rather than a level change, which
is the worse of the two and cannot be undone by moving the master. The decision
is right and the gate works. Only the argument for it was wrong, and it was
wrong in the direction that flattered it, which is the second time this project
has found that exact shape in a sentence a decision rests on.

**A warning that argued for the deletion it exists to prevent.** The loader
holds a `jlimit` and, one call further in, `legacyPositionToGain` clamps the old
position to the old travel. The comment at `ProjectIO.cpp:815` tells the next
reader not to delete the inner clamp, and then quotes 0.943820 as what would
happen if they did. 0.943820 is what the code gives **with** the clamp. Without
it a hand edited `volume="9"` reaches a gain of 126.5625 and saturates at the
new ceiling: +10 dB, where the old loader gave +7.75 dB, and the `jlimit`
cannot catch it because 1.0 is inside its own range. A reader who checked that
comment against the code would have concluded the clamp was a no-op and removed
it. This is the latency graph's fault in a new place: a comment claiming the
opposite of what the code does.

**A fade written the wrong way round.** `AutomationCurve.h` explains storing a
position rather than a gain by saying that storing the gain would make a
straight line "fast at the start and slow at the end". For a fade out, linear
in gain is the other way round: it reaches -6 dB only at the halfway point,
-20 dB at nine tenths and -40 dB at ninety-nine hundredths, so nearly all the
audible travel is in the last tenth of the gesture. True for a fade in, and the
direction that matters here is a fade out.

**A scale nothing draws.** `ceilingDb` and `floorDb` were said to be named
constants "because the interface draws the scale and the tests assert it".
Neither appears anywhere in `Source` outside `FaderLaw.h`; the only consumers
are two test files. The mixer strip is a plain vertical slider with a numeric
decibel readout and no printed ticks, so the eleven figures this law is derived
from are asserted and not drawn. The release notes promise the user a Behringer
scale beside the fader. The positions are all correct under the law, and there
is nothing there to look at. Recorded as the honest statement plus a note that
drawing it is worth doing and is not done.

**A counterfactual quoted to one digit where everything else has four.** The
figure for a migration routed through the fraction of the travel instead of
through the gain appears as "-4 dB" in four comments. It is -4.4000. In a header
that quotes 0.943820, 7.11e-15 and 13.600, a round -4 reads as a different kind
of number than it is.

**One claim that was true and unobservable.** The forward law's comment presents
the inclusivity of its segment comparisons as an asymmetry "worth being careful
about", silent "everywhere except at the breakpoint itself". It is silent at the
breakpoint too, because the law is continuous there: the segment above and the
segment below return the same double. That is why six of the ten surviving
mutations survived. Rewritten to say what is actually fragile, which is the
breakpoint value rather than the comparison: moving 0.5 to 0.55 was caught at
once.

### One test that could not fail in the direction it was written for

`Tests/AutomationCurveTest.cpp` asserts that a curve at the top of its range
reaches the top of the fader, in two ways, and its comment credited the wrong
one. `FaderLaw::positionToDb` clamps its argument before it branches, so
comparing it against `ceilingDb` answers exactly +10 dB for a position of 1, of
1.25, of 2 and of a billion alike. It catches an undershoot and is blind to an
overshoot, which is the direction a silent return of the old 1.25 travel comes
from. The comment called that check the strengthening and dismissed the
comparison against `FaderLaw::maxPosition`, which is the line that was actually
doing the work, as "two numbers in the same file agreeing".

Proven rather than reasoned: scaling both directions of
`autoControlValue`/`autoCurveValueFor` by 1.25, which restores the old travel
on both sides and is exactly the regression that would move every drawn fade,
was caught by that one line alone. The overshoot is now asserted on the raw
position where the clamp cannot reach it, and the same mutation is caught by
two checks. The comment says which does what.

### What the wiring looks like, which is clean

Audited independently, with every numeric claim rechecked here against the real
header. No behavioural fault.

- **The version gate cannot misfire in either direction.** `formatVersion = 7`
  is written in one place and read in one, both migrations gate on the same
  `fileVersion < 7`, and `load` is the only entry point and resets the engine
  and project first, so there is no import, append or autosave path that
  re-enters a migration on already migrated state. A v6 file is migrated once,
  a v7 file is read raw, an older build refuses a v7 file outright through the
  newer-than-us check. End to end: stored 0.8, old unity, comes back 0.750000
  exactly; stored 1.25, the old ceiling, comes back 0.943820, which is +7.7528
  dB, the same gain the old loader gave; stored 0 is exact silence; a missing
  attribute is unity under both versions.
- **Playback matches export.** `FaderLaw::positionToGain` is called from
  exactly one place in the engine, inside `processInsert`, which both the device
  callback and `renderOffline` reach, the stem sink included. There is no second
  fader computation anywhere. Send levels are raw linear gains that never went
  through a fader law and correctly needed no migration.
- **The audio thread is clean.** Once per insert per block, seventeen per
  block, not per sample: a finite test, a clamp, up to three comparisons and
  one `std::pow`. No allocation, lock, log or container growth. The engine
  never calls `gainToPosition`, so it never pays the `log10`: its only caller
  is the migration, on the message thread inside the loader. The new code is
  also more robust than the old on this path, because a NaN position now gives
  gain 0 rather than filling the buffer with NaN.
- **New state needs four things, and this change introduces none.** Insert
  volume lives in `AudioEngine::InsertControls`, is saved, restored, and reset
  in `resetEngine`, which both `load` and `newProject` call. It was never in
  the undo snapshot and still is not, which is right: #59 changes the meaning
  of an existing attribute rather than adding state.
- **Every call site of the old travel moved.** `kUnityFaderGain` and
  `kMaxFaderGain` are gone from `Source` entirely and no `1.25` survives
  outside comments. The slider range and double click value, the context menu
  reset, the decibel readout, the automation clamp, the atomic's default, the
  loader default, the automation lane's rest rule and both directions of the
  curve conversion all moved. The places a missed call site could have hidden
  were checked too: no modulator can be aimed at a fader, the master readout is
  a peak meter rather than a fader, and there is no second fader anywhere in
  the mixer.

### A gap worth a night of its own

**Nothing executes the migration's wiring.** The law and the migration are
tested to half a million cases each, and the decisions that call them are all
in `ProjectIO.cpp`, which needs JUCE and so is compiled by neither the test job
nor anything in the sandbox. The suite stays green if somebody drops
`fileVersion < 7` from either migration, migrates `insertPan` curves too, or
drops the curve migration entirely.

The code as merged does none of these and no path to any of them could be
constructed. But what the gate is worth is now pinned in `FaderLawTest` rather
than left to a reader: a v7 file read as legacy a second time moves unity down
1.12 dB, the old ceiling down 4.88 dB and a strip at -60 dB **up** 15.71 dB, so
it is the same balance change as above and it would present as "the mixes I
saved since the update have moved" with nothing in the audio path to blame.

A small harness stubbing the three JUCE calls the volume hunk uses
(`getDoubleAttribute`, `jlimit`, `atomic<float>::store`) would execute the gate
itself. That is the recommended target for a later night, and it is the last
thing about this change that is not proven.


---

## 2026-10-27: the reduction to a word length, which only a file ever sees

### What was looked at

`Source/Dither.h`, 495 lines with no JUCE, and its wiring in
`Source/ExportJob.h`, 246 lines with no test at all. Chosen as an audit of
recently merged work: pull request #54 landed four days before this and is the
newest feature in the repository, and nothing had read it since it was
written. It is also the one place where being wrong is invisible from inside
the studio. Playback hands floats to the driver and reduces nothing, so the
only thing that ever sees a 16 bit code is the file.

Every JUCE-free header in `Source` still has a test except `Fx/Unit.h`, which
remains unreferenced scaffolding and which the night of 2026-10-21 set aside
for that reason. Not `Fx/Svf.h`, which open pull request #55 is changing, and
not `AutomationCurve.h`, `FaderLaw.h`, `AudioEngine.*`, `ProjectIO.cpp`,
`MixerComponent.h`, `PlaylistComponent.cpp`, `RELEASE_NOTES.md` or
`Research/BACKLOG.md`, which open pull request #59 is changing.

### Nothing in it is wrong today

Said plainly because it is the result. The quantiser, the dither's
distribution, the seeding, the integer and float export paths and the
preference translation are all correct, and an independent read by the auditor
found no live fault either. 38 mutations, every one of them built with the
sanitizers `Tests/run.sh` uses.

Specifically checked and sound:

- Two streams in one export can never coincide. The seeds are consecutive
  integers into SplitMix64, one per channel per file, so stream B's state is
  always stream A's plus one and the two state sets can only meet at a lag of
  the additive constant's inverse modulo 2^64, of order 10^19 samples.
  Measured at the export's own seeds over 4194304 draws: a correlation of
  -0.0007 between the master's two channels, inside the sampling spread, and
  not one identical draw at the same index.
- The dither state is continuous for the whole render and cannot restart. The
  quantisers are built once in `open` before `renderOffline` is called and
  live in a `std::map` node whose address is stable, and `renderOffline` fixes
  its block size once, so `codes.resize` happens on the first block only.
- No path writes 16 bit undithered or 24 bit dithered. One `Depth` is in play,
  everything derives from it, and `modeFor` is idempotent and applied twice.
  `describeFormat` reports the mode after the policy, so the status line
  cannot name a dither the file did not get.
- At 32 bit float, `writeFromFloatArrays` really does pass the floats through
  untouched. Read in the 9.0.3 sources: it casts straight to `const int**`
  when the writer is a floating point one, and `Int32::setAsInt32LE` is a
  32 bit copy with a byte swap on a big endian host.
- Every JUCE signature the change uses, against the real 9.0.3 headers, and
  `leftJustified` against the two narrowings it is built to undo.

### One real gap, and it is this project's recurring fault in a new place

Adding a fourth depth would have gone unnoticed, and `Dither.h` expressly
contemplates somebody adding one: its note on stored indices records 8 bit as
a candidate deliberately left out.

Three switches on `Depth` have no `default` and a `return` after the closing
brace, which exists to silence a warning about control reaching the end of a
non-void function. Its side effect is that a new enumerator falls through to
the 24 bit answer rather than failing to compile, and nothing in the project
is built with `-Werror`, so the unhandled case is three warnings in a CI log.

The test could not see it because its property loop ran over a hand written
list of three depths rather than over the depths a stored id reaches. Proven
rather than reasoned: adding `Depth::int8` at the end, with `numDepths`,
`depthAt`, `depthToStored` and `name` updated and only `bitsFor` left alone,
compiles with one warning and prints "all checks passed".

What that export would have been: the 24 bit word length, scale, rails and
shift, with the preferences menu and the export's own status line both saying
"8 bit" over a file that is 24, and `modeFor` answering none, so the one depth
that needs dither more than any other would have been written without it.
Nothing errors.

### Three claims that nothing produced

The header's figures mostly hold up. Rerun tonight, `Tests/DitherProbe.cpp`
gives 36.58 dB against "at least 36.6 dB clear" undithered, 4.06 against "at
most 4.1", and -25.36, -39.45 and -28.55 against "-25.4 at three and a half
steps, -39.5 at four, -28.6 at four and a half". Three did not.

**A swing of 7.2 dB between neighbouring tenths of a step.** The probe sweeps
nothing at tenth step spacing, so the figure cannot be read off anything it
prints. It is real over 1.5 to 8 steps, and it is 14.3 dB over the 1.5 to 64
its own table covers, so the range was the whole number: the amplitudes at
which a sine's peaks land unluckily thin out as it gets louder, so a wider
sweep has further to fall between two of them. Both spacings are swept now and
both ranges are stated. 14.09 dB between half steps over 1.5 to 16, 7.22 dB
between tenths over 1.5 to 8, 14.27 dB between tenths over 1.5 to 64.

**A mean error of -0.4997 LSB, attributed to JUCE's own narrowing.** No
measurement of JUCE's path can live in this repository, because JUCE cannot be
compiled in the sandbox. The probe now reconstructs it from the 9.0.3 sources
it cites, `convertFloatsToInts` and the two `setAsInt32LE` narrowings, and
measures -0.5000 LSB at 16 bit and -0.4981 at 24 against zero for the
quantiser in `Dither.h`. The reconstruction has to be handed a float rather
than a double, as the real one is: near full scale a float's grid is only
twice as fine as a 24 bit code's, and getting that wrong moves the figures by
sixteen points.

**A threshold of 7 dB in `DitherTest`, where the code uses 1.** The sentence
compared 7 dB against 7 dB and so said nothing. It is the one check in the
file that tells summed dither from differenced dither, which makes it the
comment a future reader is most likely to lean on.

### One thing that was a second opinion rather than a fault

`PreferencesWindow::refreshDitherEnablement` computed the dither policy a
second time, as `depth == Dither::Depth::int16`, and drove both the control's
enablement and which of two paragraphs the user reads from it. Its own comment
promises the opposite, that the box agrees with the engine rather than the
other way round, and `Dither.h` says the same from the other end: "a user
interface that merely greyed the control would leave the engine free to
disagree with it." It agreed by copying the predicate. It calls `modeFor` now.

Nothing is wrong in a built file today, because the two answers coincide. The
day the policy moves, and `Dither.h` records that Avid's own documentation
contradicts itself over whether 24 bit wants dither, the window would grey the
dither box and print "nothing to dither at 24 bit" over a file the engine had
dithered.

### Three survivors, all genuine no-ops

- **Two independent draws differenced rather than summed**, `a - b` instead of
  `a + b`. Mathematically the same thing: the same triangular density, the
  same variance, and the same flat spectrum, because the two draws are
  independent. The alternative the header actually warns about is the first
  difference of successive draws, which is a high pass, and that one is caught
  by the spectrum check, reading 1.10 where 3.0 is expected.
- **Removing `depthFromStored`'s range guard.** `depthAt`'s own `default`
  answers with the same depth, so the guard's removal changes nothing. It
  stops being a no-op the moment that default moves, so there is now a check
  that an unrecognised index reaches the default depth through `depthAt`
  itself. Moving that default to 32 bit float fails it.
- **Removing `modeFromStored`'s range guard.** Already recorded as a no-op in
  the test, with the two static assertions that fail on the day a third mode
  or a different default makes the bound observable.

### What is now checked

- The depth property loop runs over `depthAt` for every stored id rather than
  over a hand written list, and no two depths may share a word length or a
  name. That is what catches a new entry falling through `bitsFor` once
  somebody has done everything the compiler forces.
- A `static_assert` on `numDepths` fails to compile and names what to update,
  so the hand written list the reachability check needs cannot quietly become
  a lie. Checked both ways, the way `VoiceTest` handles its list of
  destinations nothing reaches yet: adding a depth without listing it fails to
  compile, and listing one whose word length was forgotten fails the run.
- The generator's own identity, pinned against the reference
  implementation's published output for a seed of zero. Two breakages that
  survived everything else, an even additive constant and a dropped final
  avalanche, are caught by it. Neither moves the density, the moments, the
  spectrum or the independence of two channels by anything a few million
  draws can measure, because what they cost is quality a long way out.
- That an unrecognised stored index reaches `depthAt`'s default, which is what
  makes the surviving no-op above a no-op.
- The old narrowing path, reconstructed and measured in the probe, so the half
  a least significant bit of direct current every export used to carry is a
  number the repository produces rather than one it quotes.

### Left alone deliberately

- **`RELEASE_NOTES.md`**, which says of the new export settings that "the
  default is still 24 bit, and at 24 bit the dither is off, so an export you
  do not reconfigure writes the same file it always did". The last clause is
  not true: the probe measures the old and new paths disagreeing about the
  code on half or more of a uniform spread of samples, always by exactly one
  LSB, which is the same half an LSB of offset the paragraph two above it
  describes being fixed. Nothing about it is audible, at 144 dB down, and the
  intent, that no setting moved under anybody, is right. Not corrected here
  because open pull request #59 is changing that file. Worth a sentence from
  whoever lands next: it is the page somebody checks when two bounces of an
  unchanged project do not compare equal.
- **`Research/BACKLOG.md`**, which carries the 7.2 dB figure as "the real
  figure is 7.2" without its range, for the same reason.
- **The three switches themselves.** Giving them a `default` that throws, or
  building with `-Werror=switch`, would be a better guard than a test, and
  `-Werror` across this project is a decision with consequences for every
  platform build rather than a hardening change. The test now fails both ways,
  which is enough to stop the fault.
- **`ExportJob`'s discarded `write` return.** A disk filling up part way
  through truncates the file and the studio still reports a successful export.
  Identical to the behaviour before #54, so not a regression, and it is one
  `if` away from being reportable. Noted for a night that is fixing rather
  than proving.
- **`getDepth` and `getDither` on `ExportJob`**, which have no callers.

### Mutation testing

38 mutations of `Dither.h`, each compiled with
`-fsanitize=address,undefined -fno-sanitize-recover=undefined` against the
suite's own flags. Against the test as it stood, 30 were caught and 8
survived. Against the test as it stands now, 35 are caught and the 3 above
survive as genuine no-ops.

Aimed at what would be a real mistake rather than at scoring: the quantiser
rounding changed to a floor and to a truncation toward zero, the dither
applied after the rounding instead of before, one draw instead of two and
three instead of two, the draw's width halved and doubled, the draw left
uncentred, the scale and both rails moved by one, the left justifying shift
moved by one, the policy relaxed to dither 24 bit and ignored altogether, the
clamp removed and its bounds swapped, the NaN guard removed, every channel
given the same stream, the generator's additive constant made even, its final
avalanche dropped, its state left unadvanced, two stored indices swapped, a
word length and a name made wrong, `isFixedPoint` inverted, `numDepths` left
one short, both range guards removed, `depthAt`'s default moved, and a fourth
depth added in the two ways somebody would really add one.

Three of the catches are worth naming for what caught them rather than for
being caught. Removing the clamp is caught by the full scale checks; removing
the NaN guard is caught by the sanitizers alone, with the program dying on the
undefined cast rather than failing a check, which is the argument for running
them; and giving every channel the same stream is caught by cancellation,
because two identical sequences subtract to exactly nothing and read as no
dither at all.

---

## 2026-10-23: comping, which is the one edit that is the performance

### What was looked at

`Source/CompModel.h`, 403 lines, no JUCE: which take of a folder is heard
where, and the equal-power crossfade at each join. Chosen because it had never
been audited, because both the playback and the export paths read it, and
because of what a fault in it costs. A comp is not a setting laid over a
performance, it is the performance, so a wrong answer here is a click in a
comped vocal or the wrong pass playing, and both are heard rather than
reported.

A priority 2 target in the brief's ordering rather than priority 1: its test
was not the thin one its line count suggested. 473 lines, 4000 randomised
comps, a unity power check at 600 points across every one of them. Not
`Fx/Svf.h`, which open pull request #55 is changing, and not `Fx/Unit.h`, which
the night of 2026-10-21 set aside for the right reason and which still has
nothing referencing it.

### Nothing in the arithmetic is wrong

Said plainly because it is the result. 52 mutations, and separately four
properties the test did not have were written as probes and run against the
code as it stands before anything was changed:

- A drag puts the take asked for across the stretch asked for and moves
  nothing outside it: 200000 randomised drags onto comps already being worked
  on, clean.
- Normalising twice is normalising once: 200000 randomised lists of rubbish,
  clean. The project relies on this, since it normalises on every edit and on
  load.
- `compTakeAt` and `buildCompSpans` never disagree about which take plays,
  being two copies of one decision: clean.
- A join is never wider than the crossfade asked for, and is exactly that wide
  wherever the spans either side can hold it: clean.

The segment cap is exact rather than off by one, and for a reason worth
keeping: the implicit boundary inserted at zero copies the take of the one it
displaces, so the merge of neighbours naming the same take always drops the
old front, and 4096 stays 4096 even when the list that arrived started late.

### What the test could not see

**46 of 52 mutations are now caught. 36 were caught before.** The ten that got
through fall into one group: the randomised comps checked *structure* and never
*meaning*. They asked whether the result was normalised, whether the spans
tiled the folder, whether the joins summed to unity power. They never once
asked whether the take dragged for is the take heard.

The sharpest demonstration is one line of the header. `setCompRegion` reads
what plays at the far edge of the drag *before* erasing the boundaries inside
it, and the comment above that line says why. Read it afterwards instead and
every structural check in the file still passes: the result is normalised, it
tiles, it sums to unity. What it does is hand the stretch after the drag to
whatever was playing before it, so comping over the end of pass three deletes
the whole of pass four's stretch. A line of the vocal is gone and the
arrangement looks exactly right. The one hand written case of that shape, the
nested region, happens to put no existing boundary strictly inside the new
drag, which is the only arrangement where the two readings agree.

The others:

- **A join's width was never measured.** Every identity in the file is scale
  free: doubling every crossfade leaves the equal-power sum at unity, the
  windows disjoint and the reads in range. A folder asking for ten
  milliseconds could have been given twenty at every join with nothing to say
  so.
- **`compTakeAt` was never checked against the spans**, though they are two
  copies of one decision and the engine reads one while the playlist draws
  from the other.
- **A read range was never checked for running forwards.** It can only run
  backwards where a take's audio misses its span entirely, which the
  randomised cases produce often, and nothing downstream is written expecting
  a length below zero.
- **A crossfade that is not a number was never fed in.** Unreachable through
  the project, where both the loader and the setter clamp to nought to 250 ms,
  but the guards in `buildCompSpans` are real and a negative half width pulls
  every read range inward from its own boundary rather than past it: a short
  silence at every join, in the one calculation whose entire purpose is that
  there are none.
- **A boundary sitting on the folder's own end was never fed in.** Only a file
  written by something else can produce one, which is exactly the case the
  guard is for, and kept it asks for a final span of no length.
- **The later of two edits on one spot was checked on three segments.** The
  rule is implemented by sorting stably and letting each duplicate overwrite
  the one before it, and on a short list an unstable sort is stable anyway:
  libstdc++ insertion sorts anything up to sixteen elements. `std::sort` in
  place of `std::stable_sort` passed, and would give a different comp on a
  different standard library. The same fault `PanOneBreed.h` carries its own
  generator to avoid, arriving by another route: a project that opens
  differently depending on who built the studio. It is now checked on sixty
  four doubled boundaries.

### One comment was claiming a reason that does not apply

Not a fault in the arithmetic, and recorded because this file keeps meeting
the shape. The removal of non-finite boundaries in `normaliseCompSegments`
said it was there to keep a NaN out of the sort comparator. The floor on every
position runs before the sort, and `std::max (0.0, x)` returns its first
argument when the comparison is false, so a NaN reaches the comparator as
nought either way. The stated reason does not apply.

The real one runs rather than being undefined, which makes it worse. A
boundary with no position lands exactly on the folder's start carrying
whatever take it named, and takes over the opening of the performance; an
infinity below zero does the same. The comment now says that, and the test
checks the list that comes back rather than only its invariants, because a
boundary at zero naming a take that exists satisfies every invariant a
normalised list has.

### Three survivors that reasoning had wrong

Worth recording as a method note. Nine mutations survived the strengthened
test, and all nine had been reasoned through and written down as genuine
no-ops. Diffing each mutant's whole output against the original's over a
deterministic sweep of 30000 comps found that three of them changed it.

- **The NaN removal**, above.
- **The clamp of a drag into the folder** and **the floor on a drag's width**,
  both of which looked redundant and are not. An untouched comp is an empty
  list, meaning "the first take, all the way through", and normalising an
  empty list spells that out as one segment at zero. The two are the same
  performance, so no check on which take is heard can tell them apart. What
  tells them apart is `TakeFolder::operator==`, which decides whether the
  project is dirty and whether an edit reaches the undo stack: without those
  guards a click that misses, or a drag off the end of the folder, pushes an
  undo step for an edit that did not happen, against the rule that undo is one
  step per finished edit rather than one per mouse movement. Every other test
  in the file drags on a comp that has already been normalised, where both
  spellings have collapsed to the same one, which is why the case was invisible.

Reasoning about a no-op is not the same as measuring one. The diff is cheap
and it should be the step, not the argument.

### The six genuine no-ops

Each measured byte for byte identical over the same sweep, not merely
unobserved.

| Mutation | Why it changes nothing |
|---|---|
| The folder's end is allowed a fade out | `boundary()` saturates at the folder length, so the span past the last boundary has zero width and the half width is zero regardless. Two independent mechanisms prevent a fade there |
| The take index is not clamped before indexing the takes | `normaliseCompSegments` has already clamped every index below the take count, and does so against `min (numTakes, kMaxCompTakes)`, which stays in range even for a list longer than the cap |
| The closing boundary is pushed even at the folder's end | Normalising erases any boundary at or past the end, so pushing it unconditionally is the same as not pushing it |
| A boundary exactly at the far edge of the drag survives the erase | What plays after the drag is read before the erase, so the boundary pushed at that edge carries the same take, and the later-wins rule collapses the pair |
| The renumbering uses `>=` rather than `>` | The preceding branch has already consumed the equal case, so the two conditions are the same condition |
| The orphaned stretch's fallback take is not clamped | `compTakeRemoved` normalises before returning, which clamps it to exactly what the explicit clamp would have given |

All six are defence in depth rather than dead code, and three of them are the
second of two guards on the same thing. They stay.

### What is now checked

`Tests/CompModelTest.cpp`, 473 lines to 861, still 5 seconds under the
sanitizers. The failure messages in the randomised loops are built only when
something is wrong: four million eager concatenations cost six times the rest
of the file put together.

- **What a drag means**, over 20000 randomised drags onto comps already being
  worked on: the take asked for is heard across the stretch asked for, and
  everything outside it reads exactly what it read before.
- **A drag that does nothing leaves an uncomped folder uncomped**, for five
  shapes of nothing: off each end, a click, a drag narrower than the tenth of
  a millisecond that is the floor, and one starting exactly at the end.
- **A join is the width it was asked for**, never wider, and exactly right
  where both spans either side can hold it.
- **`compTakeAt` and the spans agree**, on every span of every randomised comp
  and after every take deletion.
- **No read range runs backwards.**
- **A crossfade of NaN, of either infinity, and of two negative widths** gives
  no join at all rather than a hole at every join, with unity power checked at
  4000 points across the folder for each.
- **A boundary on or a hair before the folder's end is dropped**, for five
  positions around it.
- **A boundary whose position is not a number is dropped rather than moved to
  zero**, checked against the list itself.
- **The later of two edits on one spot wins** on sixty four doubled
  boundaries, which is long enough that an unstable sort reorders them.

---

## 2026-10-21: the one pole section

### What was looked at

`Source/Fx/OnePole.h`, 203 lines, no JUCE. It was chosen for the oldest reason
in this file: the header claimed at line 44 to be "Verified in `OnePoleTest`",
and no `OnePoleTest.cpp` existed. What covered it was three functions inside
`Tests/PassFilterTest.cpp`, which is a test of the cascade built on top of it.

What sits on it is the reason it was worth a night. `Fx/PassFilter.h` uses it
for every odd order, so a six decibel per octave high pass, which its own
header calls the single most used filter in a mix, is this section and nothing
else. The header names the phaser's first order allpass and the gentle shelves
as the other two jobs only it can do.

### One real fault, and it is a blast of noise

**A corner set at one sample rate and then read at a lower one puts the pole
outside the unit circle, and the section runs away to infinity.**

The corner was clamped against Nyquist once, in `set`, using the sample rate as
it was at that moment. The rate outlives that call. Handed a corner near the
top of a 192 kHz band and then told the rate is now 48 kHz, the section kept a
corner past the new Nyquist, and the tangent of a frequency past Nyquist is
negative: `c` came out negative with it and the pole landed at 1 - 2c, outside
the circle.

Measured, every one of the five responses reaches a non-finite output inside
400000 samples, at every pair of rates tried: from 96 or 192 kHz down to 8,
22.05, 44.1 or 48. It is not a drift. It is a few seconds of doubling, and
then the output is whatever infinity sounds like through a converter.

Nothing reports it, which is the shape this file keeps recording. The driver
change that caused it was minutes earlier, and the symptom is in the mixer.

Not reachable today. The only thing using `OnePole` is `PassFilter`, which
re-sets its sections from the frequency it was asked for whenever the rate
moves, so the stale corner never survives. It is reachable by anything that
holds a section and is told the rate has changed, which is exactly what the
header says the phaser and the shelves are going to do.

**Fixed** by clamping on the way to the coefficient, in `update()`, rather than
once in `set`. `frequency` now keeps what was asked for and the ceiling is
applied against whatever the rate is by then. That also fixed an ordering
fault that was sitting next to it: the one hertz floor and the ceiling were
applied as one expression, so below about two hertz of sample rate the floor
was itself above the ceiling. The floor is applied first now and the ceiling
last.

**`Svf.h` has the identical shape**: the same clamp, in the same place, with
the same `setSampleRate` that calls `update()` without re-reading it. It is
not reachable there either, because `LowPassGate` and `Voice` both re-set the
cutoff on every block, but it is the same latent fault in a file that far more
of the studio runs through. It wants its own night rather than being dragged
into this one.

### One more fault, smaller and of the same family

**An unconfigured section gave two different wrong answers.** `c` started at
zero, so `process` returned silence for ever while `magnitudeAt` took its
degenerate branch and reported unity. The members said a one kilohertz low
pass at 44.1 kHz and neither answer was that, and the two did not agree with
each other.

**Fixed** by building the coefficients in a constructor. The useful
consequence is that `c` is now strictly between nought and one at every
setting of all three arguments, which makes the degenerate branch in
`magnitudeAt` genuinely unreachable rather than nearly so. See the survivors
below.

### What the old coverage was missing

It caught more than it looked like it would: every breakage of the recursion
in `process` is caught, through the order one path in
`runningItMatchesTheCurve`, and the coefficient algebra is caught by the
magnitude checks. Three things were not covered at all.

**The direction of the phase.** The existing check said in its own comment
that "the direction is asserted rather than the amount", and then asserted
`std::fabs (all.phaseAt (...))` against pi, pi/2 and zero. Taking the modulus
is precisely the direction being thrown away. Negating the whole of `phaseAt`
passed, and the header says the other sign "is also an allpass and it puts the
notches somewhere else".

**`phaseAt` against anything that runs.** `magnitudeAt` carries a comment
about a curve drawn from a second copy of the algebra being a second copy that
can disagree, and `magnitudeAt` was checked against the running filter.
`phaseAt` was a second copy that nothing had ever compared against a sample.
The sharpest demonstration: building the high pass inverted, `m0 = -1` and
`m1 = 1`, leaves every magnitude in the file identical to the last bit and
turns the phase by half a cycle. It passes the old coverage completely. In a
mix that is a six decibel per octave high pass that flips the polarity of
whatever goes through it, which cancels against any parallel path.

**Every clamp and guard in the header.** All seven could be removed without a
single check noticing, because none of them is reachable through `PassFilter`,
which clamps before it, and nothing else drove the section directly.

**The complementary check was a tautology.** It computed
`1/(1+s) + s/(1+s)` and compared it against one. That is an identity in `s`
and is true however either section behaves; neither filter appeared in it.

### What is now checked

`Tests/OnePoleTest.cpp`, 1100 lines. Every check drives the running section
and reads the complex gain back out of its output, which is the only way to
say anything about `phaseAt` at all.

- **All five responses against the specification each one is named by**, in
  phase as well as magnitude, over 240 randomised settings at four sample
  rates: 2880 readings, worst off by 2.9e-14 of its own size and 2.2e-14
  radians. The closed forms are written from the specification rather than
  from `update()`: a one pole shelf is `(A + jw)/(1 + jw)` with its half gain
  point at the corner, which is what fixes the prewarping, both output
  coefficients and the recursion together.
- **Both drawn curves against the same measurement**: 2.5e-14 dB and 1.2e-14
  radians.
- **The allpass turns the phase positive and falling**, measured off the
  output: half a cycle at the bottom, a quarter at the corner, nothing at the
  top, with the direction asserted rather than its modulus.
- **The two pole prewarping has a signature.** Dividing by the fourth root of
  the gain rather than the square root is the obvious thing to write, because
  it is what every two pole shelf in the project does, and it leaves the
  shelf a shelf with the right two ends. All that moves is the corner, which
  is the number on the control: at eighteen decibels of boost it reads
  12.371 dB where it should read 9.
- **The pair adds back up to the input on the running sections**, to 1.1e-16.
- **Sweeping the corner under a steady signal does not move the output**, by
  1.8e-15 of its own level across the whole band in both directions. That is
  the whole of the topology preserving claim: the state of a trapezoidal
  integrator is the integrator's own output and does not depend on the
  coefficient, so with the input held still the corner can be put anywhere
  without a click.
- **Every setting is stable**, read as the envelope of an impulse response
  shrinking from one window of 20000 samples to the next. A ratio rather than
  a floor at some sample number, because a corner of one hertz at 192 kHz is
  perfectly stable and still a thirtieth of its peak after a third of a
  million samples, while a corner left at Nyquist rings at exactly constant
  amplitude for ever and only a ratio sees that. Includes the rate change that
  found the fault above.
- **The corner is three decibels down where it was asked for**, from one hertz
  to 0.489 of the rate, within 4.3e-8 dB, and clamped rather than honoured
  past the ceiling.
- **An unconfigured section is the filter its own defaults describe**, both
  drawn and run, to zero.
- **Nonsense in the rate, the corner and the gain at every type** stays
  finite and bounded, the corner stays three decibels down, the curve does not
  turn back on itself, and an allpass stays flat.

It runs in 2.3 seconds with the sanitizers on.

### Mutation testing

Forty eight deliberate breakages of `Source/Fx/OnePole.h`, each compiled with
`-fsanitize=address,undefined -fno-sanitize-recover=undefined` exactly as
`Tests/run.sh` has them. **Forty five were caught.** All three survivors are
genuine no-ops rather than gaps, and all three are now written down in the
header.

- **The degenerate guard at the top of `magnitudeAt` removed.** It is a
  backstop that nothing reaches. With the corner floored at one hertz and then
  held under 0.49 of the rate, the tangent is strictly positive and finite and
  `c` is strictly between nought and one at every setting, so the branch never
  fires and removing it changes no measurement. That is an argument, so the
  test turns it into a reading: it asserts the corner is three decibels down
  whatever nonsense it was given, at every type and every rate, and three
  decibels down is not the flat unity this branch returns. The branch was
  reachable before this night, by an unconfigured section, and the constructor
  closed that.
- **A negative frequency no longer folded to DC, in `magnitudeAt`**, and
  **the same in `phaseAt`**. A negative frequency is not a request either can
  answer: a magnitude is even in it and a phase is odd, so folding to DC and
  reflecting are both defensible and neither is what a curve is for. Nothing
  asserts which happens, deliberately. What the test does assert is that the
  answer stays finite and the magnitude stays positive.

### Checked and found sound

- **The recursion itself**, which the old coverage already proved and which
  nine further breakages confirmed: the integrator, the state update, the two
  output coefficients and the normalisation of `c`.
- **The shelf prewarping in both directions**, by the square root of the full
  gain, against the half gain point of `(A + jw)/(1 + jw)` worked out from the
  specification. The header's arithmetic about it is correct, including the
  claim that a two pole shelf uses the fourth root.
- **Smith's AP1 is the sign the header says it is**, and the high pass minus
  twice the low pass really is it.
- **`reset` is exact** and nothing else moves the state.

### Also corrected

`Tests/PassFilterTest.cpp` kept the check that lied about asserting the
direction of the phase, and the tautology. The modulus is gone, so those three
assertions now say what their comment says, and the tautology is gone with a
note saying where the question is actually asked. Its header now points at the
section's own file.

### Left alone deliberately

- **`Svf.h`'s identical clamp**, as above. Its own night.
- **Both negative frequency folds**, as above.
- **`Fx/Unit.h`**, 234 lines with no JUCE and no test, which looked like a
  target until it turned out that nothing in `Source` or `Tests` references it
  at all: it describes itself as the shape every effect has, alongside an
  `Fx/Plugin.h` that does not exist yet. It is scaffolding for work in
  progress rather than code anything runs, so a test over it tonight would
  have been a test of a decision somebody else is still making.

---

## 2026-10-14: the oversampler, at every sample rate a device can open at

### What was looked at

Every JUCE-free header in `Source` already has a test, so the choice was the
test that looked weakest rather than the header that had none.
`Fx/Oversampler.h` had the lowest ratio of test to source of any DSP header in
the project, 398 lines against 424, and everything nonlinear in the studio
sits on top of it: the wavefolder, the saturator and the instrument's whole
oversampled section. Its own header says a poor filter here "leaks the
aliasing straight back in, and the oversampling then costs four times the
arithmetic for nothing", which makes it the one place where being wrong is
both invisible and expensive.

It was weaker than it looked in one specific way, and it is the reason both
faults below went unseen: **it measured at 48 kHz only**. The filter
specification is derived from the base rate, so one rate proves one rate.

### Two real faults, both silent

**1. At any sample rate of 40 kHz or below there was no antialiasing filter at
all.**

Each stage's stopband has to start at its own output rate minus the passband
edge, because content just above that is what folds to just below 20 kHz. With
a fixed 20 kHz edge that specification inverts as the rate comes down: at
40 kHz the stopband would start at 20 kHz, which is where the passband ends,
and below that it would start below the passband. `design` hit its own
`! (stopband > passband)` guard and returned an empty tap vector, and
`Fir::process` returns its input unchanged when it has no taps. So the
outermost stage became a wire in both directions: `upsample` zero-stuffed
without interpolating and `downsample` threw away every other sample without
filtering.

Measured, at a 32 kHz base rate and four times oversampling, a tone injected
into the oversampled stream at 19.25 kHz came back into the audible band at
**0.00 dB**, which is to say entirely. 22.05 and 16 kHz read the same. At
16 kHz both stages collapsed and `latencySamples` reported **minus three
quarters of a sample**, because the length of an absent filter is zero and the
arithmetic takes one off it, which the studio's delay compensation would have
taken at its word.

This is the shape `CLAUDE.md` calls a guard that defeats the feature it
guards, exactly as `playable(0.0)` did. The guard was correct about the
specification being impossible and wrong about what to do next.

Reachable: the sample rate is whatever the device hands over. JUCE's device
selector offers every rate the driver reports, and a laptop's own converter or
a shared mode WASAPI endpoint configured for a headset will offer 32 kHz or
lower without being asked. Nothing in the studio refuses one.

**Fixed** by deriving the passband edge from the rate instead of fixing it.
`passbandEdgeHz` inverts Kaiser's tap estimate against `mostTaps` and
`stopbandDb`, which between them fix the narrowest transition that can be
turned over at the depth this design claims, and takes the edge as high as
that allows. Above about 42 kHz it never binds, so the edge is 20 kHz exactly
and **nothing about 44.1 kHz or above changes at all**. Below it the edge comes
down with the rate: 15.2 kHz at a 32 kHz rate, 10.5 kHz at 22.05 kHz. A low
rate gives up a little off the top of its own band, which it can afford,
rather than giving up the thing that makes oversampling worth doing. The test
now asserts the edge is 20 kHz for every rate at or above 44.1 kHz, so a later
change that narrows the passband at the rates people actually use is a failure
rather than a surprise.

**2. The inner stages reached 82 dB where the design claims a hundred.**

`fewestTaps` was 16. Kaiser's tap estimate is asymptotic and the inner stages
have an enormous transition to make, so the estimate asked them for seventeen
taps, which is where it stops delivering what it promises. Measured at the
stop edge, which is the one frequency that matters and also the one where the
estimate is least accurate, the worst fold into the audible band was
**-82.2 dB at 88.2 kHz by four times** and the same at 44.1 kHz by eight. That
is below the test's own -90 dB bar, so this was not a claim that needed
reinterpreting: it was a failure the test could not reach.

**Fixed** by raising `fewestTaps` to 24, which is where the measurement says
the estimate becomes trustworthy. Every rate from 22.05 to 192 kHz and every
factor from two to eight now measures **-91.9 dB or better**, worst at 48 kHz
by a tone at 28 kHz. It cost eight taps on the cheap stages and nothing on the
expensive one, which never reaches the floor.

### One claim corrected rather than made true

`stopbandDb` said a hundred decibels, described as a figure that measures. It
does not: it is the figure Kaiser's window parameter and tap estimate are both
taken from, and the real worst case at the stop edge is 91.9 dB. Designing for
110 instead was measured too and brings the worst case to 97.8 dB for about
twelve per cent more taps. It was not taken: six decibels at one frequency, a
hundred below the signal, is not worth that where the oversampler is already
the largest single cost in the instrument. The header now says which number is
the design figure and which is measured, and the test prints the measured one
on every run.

The comment above `passbandHz` also claimed the turnover happens between
20 and 24 kHz, which is the Nyquist of a 48 kHz rate rather than where the
stopband actually starts, 28 kHz. Corrected.

### Two gaps in the test that no fault had found yet

**The per-phase normalisation of the interpolating filter was invisible.**
`CLAUDE.md` records it as a real past bug worth five parts in a million on a
constant, and nothing checked it. It cannot be seen in the round trip, because
the ripple it produces sits at half the oversampled rate, exactly where the
decimation filter is designed to stop. It is visible only in the oversampled
stream, which is also the only place it matters, since that stream is what the
nonlinearity reads. There is now a check that a constant 1.0 comes out of the
interpolator as a constant: it measures 8.9e-16, pure rounding, and the
documented bug reintroduced measures 1e-6.

**The reported delay was held to one sample where the code is exact to a
tenth of a picosecond.** A cascade of linear phase filters puts the centre of
energy of an impulse exactly on its group delay, and the agreement measures
1.1e-13 samples. A tolerance of one sample let three separate half-sample
mistakes through. It is now 1e-4.

### Mutation testing

Thirty five deliberate breakages of `Fx/Oversampler.h`, each compiled with
`-fsanitize=address,undefined -fno-sanitize-recover=undefined` exactly as
`Tests/run.sh` has them. **Thirty three were caught.** Both survivors are
genuine no-ops rather than gaps, and both are now written down in the header
so nobody goes looking for the test that should have caught them:

- **Letting the odd-tap step walk past `mostTaps`.** The clamp to the ceiling
  is followed by a step up to an odd count, which can land one past the
  ceiling, and coming back down by two is a backstop on a path nothing takes:
  with the passband edge now derived from `mostTaps`, the estimate never
  reaches the ceiling at any rate. Unreachable, so unfailable.
- **Aiming every stage's stopband at Nyquist rather than at the rate minus the
  edge.** The header records this as having measured -34 dB, and that figure
  is for a fixed number of taps. The taps here are not fixed, so asking for a
  stopband that starts lower and letting the estimate pay for it is not worse,
  it is twice the price: 154 taps at 48 kHz rather than 77, for a filter that
  stops 4 kHz lower than anything needs. A performance regression and nothing
  else, which no measurement of quality can fail.

### Checked and found sound

- **Every caller guards a refused `upsample`.** `downsample` has no size check
  of its own and does not need one: `Saturator::process`, `WaveFolder::process`
  and `Voice::throughTheNonlinearSection` all return on a null return from
  `upsample` before reaching it.
- **The buffer swap in `upsample`.** The data the caller modifies is in the
  vector `scratch` names after an odd or an even number of swaps alike, and
  `downsample` reads that one. Both buffers are the same size, so swapping
  them cannot leave either short.
- **`latencySamples` against the rate each stage runs at.** Measured against
  an impulse at eight rate and factor combinations and exact at all of them.

### Left alone deliberately

- **The CPU cost at a low rate.** The outermost filter at a 32 kHz rate is 255
  taps rather than 77, so the oversampler costs roughly twice there. Correct
  and slower beats wrong and fast, and the rate is an unusual one. If it ever
  matters, the lever is the passband edge rather than the stopband depth.
- **Designing for 110 dB**, as above.
- **The polyphase form.** `CLAUDE.md` names it as the obvious next step: the
  interpolating filter multiplies by a zero every other tap and the decimating
  one computes an output that is thrown away, so each is exactly twice the
  work it needs to be. That is a performance change, not a hardening one, and
  it would want its own night and its own before and after measurement.

## 2026-11-18: the tuner's load bearing numbers, which nothing reproduced

Picked from `Research/BACKLOG.md`'s own list of what was left undone, where it
is stated plainly:

> **The reading cost table and the tear rate figures are still not
> reproducible from anything checked in.** `CLAUDE.md` names `*Probe.cpp` as
> the mechanism for exactly this and there is no `TunerProbe.cpp`. These
> figures are load bearing: they set the duty cycle, which sets the reading
> rate, which decides the whole vibrato result.

This is category 4 of the hardening brief: a claim stated as measured, with no
measurement in the tree that still produces it.

What is claimed and by whom:

- `Tuner.h`, above `Pacing`: a four row table of what one reading costs, 9 ms
  at 44.1 kHz, 11 at 48, 48 at 96 and 197 at 192, said to be a release build
  measurement, and the statement that the cost goes as the square of the
  sample rate.
- The same comment, derived from that table: reading rates of 8.5 a second at
  44.1 and 48 kHz, 4.5 at 96 and 1.1 at 192; mean hops of 118 ms at 44.1 and
  221 ms at 96; a quarter of a core as what `dutyMultiple = 3` holds the
  analysis thread under.
- `Tuner.h`, above `InputRing`: tear rates measured with two real threads, 38
  per cent of accepted snapshots torn at a capacity of the snapshot plus 64
  with 512 sample blocks, 29 out of 147572 at the snapshot plus 512, and none
  out of 412710 at four times the snapshot.
- `Tests/TunerTest.cpp:2249` hard codes the four cost figures, and
  `sweepVibrato` is handed 0.009 and 0.048 out of the same table, so the
  vibrato conclusions, 4.9 cents over 14 readings and 18.5 cents at 192 kHz,
  rest on numbers nothing checks.

### What is being done

A `Tests/TunerProbe.cpp` that measures all of it, and a check in the suite for
the parts that are arithmetic over a cost rather than a property of a machine.
The absolute milliseconds belong to whatever processor runs the probe, so the
machine independent claim, that the cost goes as the square of the rate, is
what the probe holds to a bound, and the derived rates are checked against
`Pacing` itself.

Then mutation testing of whatever is added.

### The fault

**`InputRing`'s tear check was blind to one audio block at every ring size, and
the header called the thing that was supposed to close that a correctness
requirement.** A torn snapshot reaching the estimator is a discontinuity in the
middle of the buffer, which YIN reads as a perfectly plausible and completely
wrong period: the tuner shows a confident note that is not the one being
played, with nothing on screen to say so.

The arithmetic. The reader takes `before`, copies `[first, first + want)` where
`first = before - want`, then takes `after`. Call the writer's true position,
including the block it is partway through, T. The counter is published after a
block rather than during it, so `after <= T <= after + block`. The copy survived
only if the writer never came back round onto `first`, which is
`T - first <= capacity`. Comparing `after - first` against the capacity, which
is what it did, therefore accepts everything up to `T - first <= capacity +
block`: a window one block wide, at **every** capacity.

The header instead carried

    capacity >= snapshot + 2 * largest block the callback will deliver

and called it a correctness requirement, with tear rates quoted beside it as
though the requirement were what the rates demonstrated. No capacity could have
closed a window whose width does not depend on the capacity.

Measured by `Tests/TunerProbe.cpp`, with the detection validated first against
a ring nobody was writing to (20000 accepted, 0 torn):

| capacity | accepted | torn | torn share |
|---|---|---|---|
| snapshot + 64 | 43965 | 27987 | 63.66% |
| snapshot + 1 block | 45524 | 29 | 0.06% |
| snapshot + 2 blocks, **the stated requirement** | 43129 | 6 | 0.01% |
| snapshot + 3 blocks | 59990 | 3 | 0.01% |
| snapshot + 4 blocks | 69697 | 4 | 0.01% |
| snapshot x 4 | 200000 | 0 | 0.00% |

**A rate that stops falling as the ring grows is the signature of a fixed blind
window rather than of a ring that is too small, and reading it as the latter is
how this survived two audits.** The three capacities the header quoted were two
below the requirement and one nearly four times it, so between them they said
only that a small ring tears and a large one does not, which was never in
doubt. Nothing had ever walked across the boundary the requirement names.

### The fix

Compare against `capacity - block` instead. The same algebra then gives
`T - first <= capacity`, which is the condition itself, so the check is sound at
any capacity and does not lean on the ring being generous. The ring has to be
told the largest block that can land during a copy, so `setSize` takes it and
has **no default**: the engine passes `largestStudioBlockSamples`, the same
constant the capacity is derived from, and a single threaded caller passes zero,
which is a statement about its writer and not a way of switching the check off.
The span is clamped at zero rather than allowed to go negative in int arithmetic
and wrap to the largest possible span.

The `snapshot + 2 * block` figure is kept and is still required, for the reason
it actually serves rather than the one it was given: a copy is accepted only if
the writer advanced at most `capacity - block - snapshot` during it, and the
writer advances in whole blocks, so below that a reader overtaken by a single
block is refused every time. That is the right way round. A ring too small now
reads as a needle that does not move instead of one that lies.

With the honest bound, over the same capacities, the same writer and the same
run: **zero torn at all six**, and acceptance healthy at the requirement and
above (43255 accepted at the requirement, 200000 at four times the snapshot).

**Nobody will have heard it.** The shipped ring is `studioRingCapacity()`, four
times the snapshot, where the probe found no torn snapshot in 200000 even with
the old check. It was a fault in waiting that the capacity margin was carrying,
which is why the margin is now documented as buying a needle that moves rather
than as what makes a snapshot trustworthy.

### What the probe does and does not settle

`Tests/TunerProbe.cpp` reproduces the fault through the shipped ring rather than
through a copy of it, by passing a block bound of zero, which is the old check
exactly. A probe that demonstrates a bug against its own reimplementation of the
code demonstrates nothing.

- **The window table is exact and reproduces to the sample**: 5734, 6239, 12473
  and 24939 at 44.1, 48, 96 and 192 kHz, 130 ms at every rate.
- **The reading cost table is corroborated and is machine dependent.** This
  sandbox measured 4.5, 5.4, 25.6 and 81.6 ms against the header's 9, 11, 48 and
  197. That is a uniform factor of about two at the first three rates and 2.4 at
  the top, which is one machine being faster than another rather than a figure
  being wrong. The claim a processor cannot change, that the cost goes as the
  square of the sample rate, is what the probe holds to a bound: measured 4.77x
  and 3.19x across the two doublings, against 4.
- **Every figure the comment derives from that table is right.** Checked as
  assertions in the suite now rather than printed: 8.45 and 8.48 readings a
  second at 44.1 and 48 kHz against the stated 8.5, 4.53 at 96 against 4.5, 1.10
  at 192 against 1.1, and mean hops of 118.3 and 220.9 ms against the stated 118
  and 221. The quarter of a core that `dutyMultiple = 3` is said to hold is
  21.7% wherever the duty cycle binds.
- **`studioRingCapacity` reproduces**: 99756 samples, 0.52 s at 192 kHz and
  2.26 s at 44.1 kHz, against a requirement of 41323.
- **The tear percentages themselves are a race and are not reproducible as
  numbers.** 63.66% here against the header's 38% at the snapshot plus 64, on
  two cores against however many the original had. What reproduces is the
  ordering and the zero, which is what the decision rested on. The header now
  quotes the probe's figures and says they are a race.

### Mutation testing

Seventeen mutations, compiled with the sanitizers exactly as `Tests/run.sh` has
them. Fourteen caught, three survivors, of which two were real gaps in the first
draft of the new test and are closed, and one is a provable no-op.

| # | mutation | result |
|---|---|---|
| 1 | the subtraction removed, which is the original fault | caught, including by the racing check |
| 2 | one sample too generous, `cap - maxBlock + 1` | caught |
| 3 | the clamp on the span removed | **survived**, gap, closed |
| 4 | two blocks subtracted instead of one | caught |
| 5 | `maxBlock` never stored | caught |
| 6 | `<=` becomes `<` in the comparison | caught |
| 7 | tear check becomes a did-anything-arrive check | caught |
| 8 | the check removed entirely, `return true` | caught |
| 9 | the writer publishes before it copies | caught, only by the new racing check |
| 10 | `first` one sample late | caught |
| 11 | `setSize` leaves the counter behind | **survived**, gap, closed |
| 12 | an overfilled write keeps the oldest samples | caught |
| 13 | the negative clamp on `maxBlock` removed | **survived**, provable no-op |
| 14 | `maxBlock` not updated on a resize | **survived**, gap, closed |
| 15 | `jitter` halved to 0.2 | caught by the new rate assertions |
| 16 | `dutyMultiple` 3 becomes 2 | caught |
| 17 | the nominal interval 100 ms becomes 120 | caught |

**The two gaps, and why they are worth recording as shapes.**

Mutation 3 was checked at a capacity **equal** to the block, where the
subtraction is zero whether it clamps or not, so removing the clamp passed. It
needs a capacity **below** the block, where int arithmetic goes negative and the
cast wraps to the largest possible span and accepts everything. A check that
pushes a guard to its boundary is testing the one value most likely not to
exercise it.

Mutation 14 was checked by resizing with the **same** bound twice, so a bound
carried over from the first call was invisible. It resizes with a larger one
now. Same shape: a reset is only observable against a different value.

**The no-op, mutation 13.** Clamping a negative block bound at zero looks like it
must be observable, because the span is the capacity less the block and a
negative block would add headroom. It cannot: `readLatest` refuses `want > cap`
before the span is computed, and `want` is the only thing extra headroom could
ever admit. A check asserting that a negative bound refuses the whole ring was
written, failed against correct code, and is recorded in the test as a no-op
rather than bent until it passed.

**One breakage the suite cannot reach, named so nobody hunts for the test.**
Passing a bound of zero at the engine's own call site, where a device callback
is writing, reintroduces the fault exactly. `AudioEngine.cpp` cannot be compiled
without JUCE, so no test here can see it. That is precisely why `setSize` has no
default parameter: the bound has to be said out loud at every call site, and the
one call site that matters says `Tuner::largestStudioBlockSamples`, the same
constant the capacity is derived from, so the two cannot drift apart the way the
capacity and the requirement once did.
