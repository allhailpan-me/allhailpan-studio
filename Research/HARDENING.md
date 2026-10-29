# Hardening log

What each night audited, how hard it tried to break it, what it found, and
what it judged to be a genuine no-op rather than a gap. A target recorded as
clean here does not need re-auditing unless the code has changed since.
Newest first.

---

## 2026-10-29: the loudness meter, which is the one number other people check

### What is being looked at

`Source/LoudnessMeter.h`, 409 lines, and `Tests/LoudnessMeterTest.cpp`, which
covers it. A priority 2 target in the brief's ordering: a test suspected of
being weaker than it looks. Nothing has merged since the night of 2026-10-28,
so there was no recently merged work to audit, and the only JUCE-free header
in `Source` still without a test is `Fx/Unit.h`, which two earlier nights set
aside as unreferenced scaffolding and which still has nothing including it.

Picked because of the ratio: 409 lines of source to 534 of test is the
thinnest cover of any real arithmetic header here, and because of the stake.
Every other number this studio reports is checked by the owner's own ears.
A LUFS figure is checked by Spotify, Apple Music and YouTube, which is the
whole reason for measuring it, so a meter that is half a decibel out is not
slightly wrong: it is making a claim about agreement that it cannot support,
and nothing about the number looks wrong.

Not `Fx/Svf.h` or `Tests/SvfTest.cpp`, which open pull request #55 is
changing, and not `AudioEngine.*`, `InputSource.h`, `MainComponent.*`,
`Tuner.h`, `TunerPanel.h` or `Tests/run.sh`, which open pull request #61 is
changing.

### Still to do

Mutation testing, the missing checks it calls for, and the result.

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
