# Research backlog

## Standing question: the unreleased work keeps growing, and now includes a tuner

The last tag is still **v0.7.1**, cut on 2026-10-13. Since the figure below was
written, quantise, multiple note selection and the tuner have all merged, and
the tuner is the first of them somebody would notice the absence of: it is on a
keyboard shortcut and in a menu, and a musician who records guitar has no
reason to know it exists until a build carries it.

So the question has a sharper edge than it had. Everything still applies:
the notes are written and staged rather than missing, a release spans several
nights' work by several sessions, and whoever cuts it should run the
`release-manager` agent over the whole span rather than over one change.

What was written when the count was nineteen:

The last tag is **v0.7.1**, cut on 2026-10-13, and `main` is nineteen commits
past it. Everything since has gone into the `## Since v0.7.0` section of
`RELEASE_NOTES.md`, which is the section v0.7.1 itself was cut from, so the
notes are written and staged rather than missing.

What is sitting there unreleased: the pan law and the mono panner, clip fades
and the de-click fade at every edge, the oversampler's filters at every rate,
the delay line, the equaliser's bell, solo with its send and isolate
behaviour, the playlist looping at the end of the bar, PAN One's forty five
new presets and its Grow breeding, and now the export bit depth and dither.

Nobody has been deciding not to release; each night has just not been the
night to decide it. Recorded here so it is a decision rather than a drift. A
release spans several nights' work by several sessions, so whoever cuts it
should run the `release-manager` agent over the whole span rather than over
one change, which is what that agent is for.

---

A running record of what each night looked at, what it built, what it rejected,
and why. Rejected ideas stay listed so a later night does not revisit them
blindly. Newest first.

---

## 2026-11-07: punch recording, and the crossfade that must not touch the punch

### Status: merged

Pull request **#73**, squash merged as `fdb3d1e` on 2026-11-07. The `tests` job,
`ubuntu-latest` and `windows-latest` were all green on the head that merged, and
macOS was still building, which is the case the nightly rule allows for. The
release notes went in behind it.

Punch in and punch out recording has been deferred on eight separate nights:
10-08, 10-13, 10-15, 10-21, 10-22, 10-27, 10-28 and 11-06. Every entry says
the same thing in slightly different words, and the 10-21 entry says it
best: the thing it actually needs is a published answer to what happens to
the audio already on the track, because recording here finds an empty track
rather than punching into an occupied one, so a punch take would sum with
the performance it was meant to replace. That entry named the two things to
read before choosing, Pro Tools' playlists and Reaper's "trim content behind
media items", and asked for a night of its own. This is that night.

### What was looked at

The `dsp-researcher` agent was sent at the published specifications rather
than at anybody's recollection of them. It came back with Avid, Steinberg,
Apple, PreSonus, Ardour, Cockos, Magix and Audacity, and with a list of what
it could not retrieve, which is below and matters as much.

### The blocking question, answered

**No shipping studio sums a punch take with the material underneath. Not
one.** Summing is not one of the three candidate models the earlier entries
named; it is a fourth thing that nobody ships. The three real models all
exist, in different studios:

| Studio | What happens to the audio underneath |
|---|---|
| Logic Pro | A take folder. `Overlapping Track Recordings > Audio > Cycle Off` offers exactly Create Take Folder, Create Track, Create Track Alternative |
| Pro Tools | A playlist lane, plus a continuous parent file. "Recording with QuickPunch is nondestructive" |
| Ardour | `Layered` by default, where the new region is opaque and only the top layer plays; `Non-Layered` trims the regions underneath |
| Cubase and Nuendo | A project wide choice of `Keep History`, `Cycle History + Replace` and `Replace`, where replacing is of overlapped *events* and not of files |
| Studio One | Non destructive, and auto crossfaded at its edges with the existing event |
| Reaper | Layered takes by default; trimming is the opt in named "tape mode" |

The decisive quote for the trim model is Ardour's rather than Reaper's, and
that is a correction to what the 10-21 entry assumed. Ardour publishes the
model completely, including the reversibility: existing regions are trimmed
"so that there are no overlaps", and this "does not affect the previously
recorded audio data, and trimmed regions can be expanded again at will."
(The Ardour Manual, *Track Modes*.) Reaper's own user guide chapters on
punch recording are unreachable: the PDF's text extraction stops inside
chapter 1, the Cockos wiki now serves a content free stub, and the forums
are behind a script gate. So Reaper is cited here only for the fact that
trimming is called "tape mode" and is opt in, which comes from Cockos' own
Quick Start Guide. The backlog named the less authoritative of its two
sources, and the research found the better one.

Worth keeping from Apple, because it is the strongest single statement:
Logic's **MIDI** list carries `Merge`, `Overlap` and a whole `Replace`
submenu, and its **audio** list carries none of them. The studio that models
overlapping recordings most thoroughly offers merge semantics for MIDI and
deliberately does not expose them for audio.

Three things the studios agree on, which are therefore standard rather than
a choice:

1. The punch must not sum with what is underneath.
2. The audio file underneath is not modified. Every default is non
   destructive, and where destruction exists it is a separately named mode
   documented as irreversible: Pro Tools `DestructivePunch`, Reaper's tape
   mode, Audacity's punch and roll. Ardour has no destructive mode at all.
3. The join wants a crossfade, and the studios that automate it do it
   without asking.

### What was chosen, and why

**Candidate (b), the take folder, through the comp that is already here.**
`CompModel.h` is already "which take is heard where, and the equal-power
crossfade at each join", already in seconds rather than beats, and already
survives a tempo change. A punch is exactly a three segment comp: the
original up to the punch in, the new take across the punch, the original
after the punch out. `Clip::compSpans` already turns that into reads and
`pushArrangement` already turns those into `AudioClipRT`, so **the export
gets the punch for free**, which is the same argument `CLAUDE.md` already
records for comping, and the reason this is the cheap model here rather than
merely the popular one.

The other two were rejected on the sources. Candidate (a), a destructive
trim, is what every manual warns about, and Avid's own documentation of
`DestructivePunch` is the best argument against it: it needs every file to
start at sample 0, needs each file to be at least as long as a preference
setting, and needs a `PREPARE DPE TRACKS` consolidation pass first, because
writing in place into an existing file requires the file to already
physically cover the punch range. Candidate (c), the non destructive trim,
is well specified by Ardour and would need a second parallel mechanism here
alongside the comp, for a result the comp already produces.

### The detail that would have been got wrong, and is the point of the night

**The crossfade lies entirely outside the punch range, in the old material.**
From the Pro Tools Reference Guide v6, Chapter 14: QuickPunch "writes a
pre-crossfade at the punch-in point (which occurs up to but not into the
punched region boundary)" and "a post-crossfade at the punch-out point
(which occurs after the punched region)".

So the geometry is neither of the two obvious readings. The pre-crossfade
*ends* at the punch in, the post-crossfade *starts* at the punch out, and
the punched region is left intact end to end:

```
      pre-crossfade              punched region             post-crossfade
  |<---- length ---->|                                  |<---- length ---->|
  ====================+==================================+====================
                   punch in                           punch out
   (old material)                (new material)              (old material)
```

`CompModel.h` centres its crossfade on the join: `CompSpan::fadeIn` and
`fadeOut` are half widths either side of a boundary, which is right for a
comp between two takes of the same performance and wrong for a punch. A
centred fade attenuates the first and last few milliseconds of the take that
was just punched in, and the attack is usually the reason somebody punched.
Avid's own advice warns against exactly this: set a crossfade "to prevent
clicks and pops without removing essential audio".

The fix needs no change to the comp model at all. **Put the comp boundary
half a crossfade before the punch in, and half a crossfade after the punch
out.** The centred window then lands in [punch in minus a width, punch in],
which is the published geometry, and the new take is at full gain for every
sample of the punch. It is also robust to the comp model narrowing a join
where there is no room for it: narrowing only shortens the fade and finishes
it earlier, so the take is still at full gain across the whole punch, and
what gets consumed is old material before the punch in, which is what Avid
describes anyway.

### Capture the whole pass, present only the punched span

Unanimous, and it is what makes the geometry above possible at all. Avid:
"Pro Tools begins recording a new file when playback begins, automatically
generating clips in that file at each punch in/out point", and "the complete
audio file appears in the Clips List along with the QuickPunch created
clips". Logic captures "approximately one bar before the punch-in locator"
and says why: "This allows the start point of the region to be adjusted
later, enabling you to hear the lead-in, if desired."

Three published behaviours depend on it and none is available without it:
the pre-crossfade needs new take material in a span the user did not punch;
dragging the punch open afterwards needs something to reveal; and not
starting or stopping the recorder mid pass is what gives Pro Tools
"instantaneous monitor switching on punch-out" with no dropout. That last
one maps straight onto this project's rule that the audio thread must not
allocate: open the file when the transport rolls, never at the punch point.

The happy consequence is that **the engine needs no change**. The recorder
here already writes the whole pass, and `RecordAlign.h` already lands it. A
punch is entirely a question of where the take goes and what the comp
selects, which is why this lands as arithmetic plus one branch in
`finishAudioRecording` rather than as a new record mode in the device
callback.

### The crossfade length: two published figures, and a choice between them

The 10-15 entry's note that Avid "suggests 10 ms for its punch crossfade" is
correct, and the source is now pinned. It is not the Auto Region Fade
preference: it is `Quick Punch/Track Punch Crossfade Length`, under Setup >
Preferences > Editing, Fades. Avid's current user guide says "10ms is a good
starting point". The Pro Tools Reference Guide v6 said "A good
general-purpose crossfade length for punches is 4 milliseconds". Audacity's
punch and roll publishes 10 ms as its actual default. Avid publishes no
default and no range for the preference itself, which was looked for and not
found.

So 10 ms is a choice between two published suggestions rather than a
specification, and it is recorded as such in the header. It is used as the
folder's `crossfadeMs` for a punch, where the existing comp default would
otherwise apply.

One more figure from the same v6 page, kept because it is a specification in
its own right and is deliberately not implemented tonight: with the written
crossfade set to zero, "Pro Tools always executes a 4 millisecond 'monitor
only' crossfade (which is not written to disk)". That is a real published
floor on the monitor path and this studio's monitor path is the one with the
compensation exemption on it. Noted below as left for later.

### The punch range: locked to the loop locators, which is a published default

Two camps. Cubase, Logic, Ardour and Sequoia all have a dedicated punch
locator pair. Pro Tools, Studio One and Reaper use the ordinary timeline or
loop selection: Reaper's record mode is literally named `Time selection auto
punch`, and Studio One's Auto Punch procedure sets "the Left Locator" and
"the Right Locator", which are the same two locators its loop recording
uses.

Cubase has both, and its default is the one taken here: "The punch in
position is locked to the left locator position and the punch out position
to the right locator position", with a `Lock Punch Points to Locators`
toggle that makes them independent. So **the punch range is the loop range**
for now, which is Studio One's behaviour exactly and Cubase's out of the
box, and it means no new persisted range, no new ruler lane and no new
dragging. Unlocking it is the obvious next step and is recorded below rather
than half built.

Ardour's one sided punch is worth having and is cheap: "In only" records
"from the In marker on, without an end boundary" and "Out only" records
"until the Out marker, without a beginning boundary". Both are in the
arithmetic tonight.

### Loop and punch together

Allowed, following the two studios that publish a behaviour and agree on it.
Cubase in cycle mode: "recording is stopped at the punch out position and
resumed at the punch in position", and under `Cycle History + Replace` "all
takes from the current cycle recording are kept". Logic: on each pass "only
events or audio that falls within the area defined by the autopunch locators
is recorded", and cycle mode "allows you to practice as many times as you
like before performing the final take". Pro Tools forbids the combination,
and its courseware lists "Loop playback not available" as a QuickPunch
disadvantage, but that is one against two and the two agree on the meaning:
every pass punches the same span and the passes become takes. That is what
the take folder here already does with loop passes.

### What was rejected, and why

- **Summing the punch with what is underneath.** Nobody ships it. It is the
  current behaviour by accident rather than by choice, and it is what this
  night exists to replace.
- **A destructive punch.** Rejected on Avid's own documentation of what it
  costs, above. No studio makes it the default and Ardour has removed it.
- **Candidate (c), the non destructive trim.** Well specified by Ardour and
  genuinely good, but it needs a second mechanism parallel to the comp for a
  result the comp already gives, and it would not get the export for free.
- **A dedicated punch locator pair, tonight.** The majority of the
  professional tools have one and it is the better end state, but Cubase's
  default and Studio One's only behaviour are both "the loop locators", so
  shipping that first is following a published default rather than cutting a
  corner. Deferred deliberately, with the unlock named.
- **Centring the crossfade on the punch point.** The intuitive reading, and
  it is wrong against the only published statement of the geometry. Recorded
  as rejected because it is what a later night would otherwise "simplify"
  the shifted boundaries into.
- **A Pro Tools style parent clip in a clip list.** The whole pass file is
  written and adopted, which is what matters, but exposing the parent file as
  a separate browsable object is a different feature and not one anybody is
  short of here.
- **Reaper as a source for anything but two option names.** Its punch
  chapters, its trim behind section and its overlapping items section are all
  unreachable, and the wiki is gone. Flagged so a later night does not spend
  the time again: Reaper's user guide PDF truncates after chapter 1 through
  the fetch tool, not merely before the MIDI editor chapter as the 11-05
  entry found.

### What could not be sourced, named as such

- A published default or range for `Quick Punch/Track Punch Crossfade
  Length`. Looked for in four Avid documents.
- Pro Tools' default pre-roll and post-roll values, and whether they default
  to enabled.
- Which `Overlapping Track Recordings > Audio` option Logic defaults to.
- Cubase's pre-roll and post-roll *units*, which two versions of the manual
  decline to state.
- Studio One's punch crossfade length, which the manual says is "very small"
  and does not quantify.
- What happens when a loop range and a punch range are both active, in
  Studio One, Ardour and Reaper.
- The audio version of Avid's "Punch Recording Over a Specified Range"
  topic. Only the MIDI one was reachable, so the rule that nothing is
  recorded before the punch in point is quoted from the MIDI page and is
  unverified for audio. It does not matter here, because this design captures
  the whole pass deliberately, but it is not to be cited as an audio source.

### What the audit found, and it was the whole value of the night

The `auditor` agent was sent at the crossfade geometry, which is the load
bearing claim. It found the geometry sound and found the fault one level
below it: **the geometry was being handed material measured from the wrong
place, so the ordinary punch produced silence where the line used to be.**

A pass's position inside a folder is seconds from the folder's start, and a
folder's start is its clip's start. The window measured it from where the
transport rolled, which is right for an ordinary recording because the clip
is created at the first pass's own position, and wrong for a punch because
the clip is already somewhere else and the transport rolls in a bar before
the punch. The take was placed along the folder by the distance between the
two origins. `buildCompSpans` then clamped its read to nothing, and the
punched bars went silent in both playback and the bounce, while the window
reported "Punched in over bars 5 to 7. The rest of the take is untouched."
Where the pass was long enough to cover the region anyway, it played three
bars of the wrong part of the performance at full level with a clean 10 ms
crossfade either side of it.

**The lesson is about where the line was, not what it said.** The branch had
462 lines of tested header and 710 lines of test either side of one untested
line in `MainComponent.cpp`, and that line was the only one that was wrong.
`Punch.h` was right and `PunchTest.cpp` was right, and the test encoded the
correct model of where a take sits, so it could not see the window disagreeing
with it. The arithmetic is now `Punch::placePass`, in the header, and the test
states the property rather than the algebra: a sample played at a beat ends up
at that beat. The algebra is what was wrong.

Five more, all of them ordinary things a player does.

- **Stop halfway through a punch** and the rest of it was handed to a take
  with no audio in it, so the second half of the phrase was replaced with
  silence rather than kept. The punch now reaches only where the clip is and
  where the pass is.
- **The punch range was not latched** when recording started. The ruler is
  live while the transport rolls, so dragging the loop during a take decided
  what the performance replaced, and switching Punch off mid take laid the
  pass over the old one and summed with it, which is the fault the night
  exists to remove. `CLAUDE.md` already states this discipline for the record
  alignment figure, "the way Ardour refuses an alignment change while a
  `DiskWriter` is running".
- **A looped punch gave every pass after the first no lead in**, because that
  is where the transport wraps to, so the join in front of the punch could
  only be a butt join, and `punchTake` punches the last pass. A punch
  therefore does not loop, which is the call Pro Tools makes the same way
  round. See below for what allowing it properly needs.
- **Punching into an already comped folder changed joins the producer had
  made.** A folder carries one crossfade width for all of its joins and a
  punch needs that width to be the one its boundaries were shifted by.
  Measured on a three pass folder set to 40 ms, an untouched join came back
  at 2 ms. Refused now, and the reason is in `Punch.h`.
- **`TakeFolder::longestSeconds` counted how long each take is, not where it
  sits**, which its own comment has always claimed it did. The two agree for
  a loop recording, where every pass starts at the folder's start, which is
  why nothing had noticed. A punch is made of passes that start later than
  the folder, so the tail of one sat past the end of the folder where the comp
  cannot reach it.
- **The Punch button lied after File > New and after opening a project.** Both
  paths latch the loop range without going through the branch that refreshed
  the button, so punch could be on with the button dark and the record button
  punching unannounced.

The audit also found two stale claims in this very entry, which were written
before the branch had finished and had stopped being true. They are corrected
in place above and below. That is the pattern `CLAUDE.md` names twice, and a
decision record is as able to drift as a comment.

### Mutation testing

Thirty three deliberate breakages of `Punch.h`, each compiled with the
sanitizers exactly as `Tests/run.sh` has them, because a harness weaker than
the real test run has already misled an audit on this project.

**Thirty two caught, one survived**, and the survivor is established as a
genuine no-op rather than a gap. Among those caught: the real fault above,
reproduced deliberately as "measure from the roll in rather than from the
clip"; the centred crossfade, which is the intuitive reading of the geometry;
the shift in the wrong direction at each end; both ends of the reach clamping;
`applyTo` not setting the folder's width, and setting it in seconds instead of
milliseconds.

The survivor is the `std::isfinite` guard on the requested crossfade width.
Removing it changes nothing, because a NaN reaches `std::max (0.0, x)` as
nought (the comparison is false and the first argument comes back, which is
the subtlety `normaliseCompSegments` already has a paragraph about) and an
infinity is caught by the first `std::min` after it. It is kept because it
says what it means, and it is marked in the header as a deliberate no-op.

An earlier run had four survivors. Two were the `std::max (0.0, ...)` floors
on the width caps, which the reach clamping had made unreachable three lines
further up: they were deleted rather than defended, since two no-ops guarding
an invariant that is stated directly above them are worse than neither. The
fourth was the folder's start being dropped from the reach, which needed a
punch beginning before the clip, played by a pass that began 5 ms before that:
measured from the wrong one, the only width the folder has comes back at 5 ms
instead of 10. There is a test for it now.

### Deliberately left for later

- **Unlocking the punch range from the loop locators**, which is Cubase's
  `Lock Punch Points to Locators`. `Punch::Range` already takes a punch as
  two numbers and carries Ardour's one sided cases, so this is a persisted
  pair and a ruler lane and nothing else. `Project::punchRange` is the only
  thing that assumes the pair.
- **A lead in for every pass of a looped punch**, which is what allowing loop
  and punch together needs. Cubase and Logic both allow the pair and agree on
  the meaning, each pass punching the same span into a folder of takes, so
  this is the better end state and the research is already done. What it needs
  is in the recorder rather than in the punch: a pass after the first begins
  where the transport wrapped, and the audio in front of it is the tail of the
  pass before, which `RecordAlign.h` deliberately leaves in that pass's own
  file rather than duplicating. Until that changes, a punch does not loop.
- **The 4 ms monitor only crossfade** at the punch boundaries, from the v6
  Reference Guide: with the written crossfade set to zero, "Pro Tools always
  executes a 4 millisecond 'monitor only' crossfade (which is not written to
  disk)". It belongs on the monitoring path, which is the one with the delay
  compensation exemption on it, and it is a real published behaviour rather
  than a nicety.
- **Post-roll, and Cubase's prerequisite for it.** Post-roll has no meaning
  unless the transport stops by itself, which is why Cubase's own procedure
  begins "Activate Stop after Automatic Punch Out". Worth keeping together
  when either is built. Note that the lead in built tonight is the roll back
  half of pre-roll and not the whole feature: there is no pre-roll amount to
  set, and Cubase treats "roll the transport back" and "keep a buffer of input
  before the record point" as two separate published features.
- **Punching into a clip that is already comped**, which is refused tonight
  for the reason above. It wants a crossfade width per join rather than one
  per folder, which is a change to `CompModel.h` and wants its own night.
- **A width per join would also make the punch's guarantee permanent.** It
  holds at the moment of punching rather than for ever: `Project` already has
  a `setCompCrossfadeMs`, with no caller outside itself today, and the day
  anything wires a control to it, widening a punched folder takes the fade
  back into the punch. Measured at 1.8 dB off the attack at 50 ms where 10 was
  asked for.
- **Punching more than one input at once.** `finishAudioRecording` is called
  per input, with the armed track for the first and a track found empty for
  each of the rest, so only the first player's part can be punched. Right for
  the case somebody is in, and worth knowing before tracking a band with
  Punch left on.

---

## 2026-11-06, later the same day: the question nine days of audits had not asked the tuner

The entry below records the tuner merging, and that its audits "cost more than
the building did". They did, and they were still missing two faults, which this
session found by picking the branch up to merge it and auditing it once more
before doing so. Both were already in `main` by the time the audit reported,
because another session merged #61 while this one was working on it. So this is
a fix on top rather than a change to an unmerged branch, which is the honest
framing and also a note about two sessions working one branch.

The question neither earlier audit asked, and it opens both faults: **what is
the real interval between readings, and what happens if the pitch is moving
periodically rather than monotonically.** Every signal in the test was steady
or moving one way.

### Fault one: the gate's criterion is a rate, and it divided by a constant

`Analyser` shows a number once the pitch is moving slower than 1.3 cents a
second. The span it divided by was `(frames - 1) * frameSeconds`, configured by
the caller, and the header was emphatic: "this is the one figure in here the
caller has to get right".

The caller was getting it wrong. `TunerPanel` passed the nominal tenth of a
second at every sample rate, and the real interval is

    spent + max (nominal - spent, 3 * spent)

because above 48 kHz a reading costs more than the whole nominal interval and
the duty cycle takes over. Against the reading costs already recorded below,
the real interval is 100 ms at 44.1 and 48 kHz, **192 ms at 96 kHz** and
**788 ms at 192 kHz**. So the gate was told 0.1 when the truth was 0.192 and
then 0.788, and every rate it reported, `movingCentsPerSecond` included, was
1.92 and then 7.88 times too large.

Three things worth keeping:

- **The direction was the safe one**, which is why two audits passed it. An
  overstated rate makes the gate stricter, so the cost was a player waiting
  0.4 s longer at 96 kHz and 0.8 s at 192, and nothing read wrong. It is still
  a fault, because the sign was all that protected it: anything that ever made
  the real interval shorter than the nominal, a more responsive needle or a
  faster machine, opens the gate early and reads sharp, which is the exact
  failure the gate exists to prevent.
- **The test could not see it.** It set `frameSeconds = 1 / fps` and then
  advanced the signal by `frameSeconds * sampleRate`, so the configured figure
  and the real one agreed by construction in every case it swept. A test that
  derives its input from the parameter it sweeps is sweeping nothing.
- The pacing lived in the window, where nothing tests it, which is the shape
  `CLAUDE.md` already records twice for the record alignment.

`process` now takes the elapsed seconds per call and sums the intervals in its
window, so the span is measured instead of assumed.

### Fault two: a sampling of the pitch cannot see a modulation at its own rate

The gate reads the spread of the last four readings. Those readings are a
sampling of the pitch, and a sampling at a fixed interval is blind to any
modulation whose period divides that interval: every reading lands at the same
phase, the four agree to a hundredth of a cent, and the display opens bright and
settled on a note moving by tens of them.

Measured on what shipped, a 220.000 Hz note with a 20 cent vibrato at the
reading rate: **19.6 cents sharp**. A singer would flatten a correct note by a
fifth of a semitone on the strength of it. The turning points of a vibrato are a
second way in, because there the pitch really is momentarily still.

And the reading rate is where it should not be, because of the duty cycle:
8.5 a second at 44.1 and 48 kHz, which is the top of the vibrato band, 4.5 at
96 kHz, which is the middle of it, and 1.1 at 192 kHz.

**What was tried, with the measurements, so nobody re-derives it.** The sweep is
the vibrato band at depths of 2, 5 and 20 cents; worst displayed error and how
many readings were shown at all:

| | worst shown | readings shown |
|---|---|---|
| fixed interval, one agreement (what shipped) | 15.7 cents | 336 |
| dithered interval, one agreement | 19.4 cents | 49 |
| fixed interval, three agreements | 15.7 cents | 312 |
| dithered, three agreements (the fix) | 4.9 cents | 14 |
| dithered, two agreements | 4.9 cents | 23 |
| a fixed `1 + jitter/2` multiplier instead of a draw | 16.5 cents | 252 |
| a twelve times weaker dither, three agreements | 4.8 cents | 86 |

The lesson is the first two rows. **Dithering the interval alone makes the
worst reading worse**, which was not the expectation: it shows far fewer
readings and the one it eventually finds is a worse one, because the gate is
re-evaluated on every reading and only has to be fooled once. And asking for
three agreements alone does nothing at a fixed interval, because the
coincidence repeats by construction. Neither half is worth anything without the
other, and knowing that needed all four corners measured rather than two.

The wider sweep, three seeds, three phases, twenty one rates and six seconds a
run, which is too slow for a suite that runs on every push:

| reading cost | device it corresponds to | worst shown |
|---|---|---|
| 9 ms | 44.1 kHz | 4.9 cents |
| 48 ms | 96 kHz | 5.0 cents |
| 197 ms | 192 kHz | **18.5 cents** |

**So it is not fixed, and the headers say so in those words.** What is shipped
is a tuner accurate to a fraction of a cent on a note held steady, which is how
a tuner is used, and foolable by a deliberate vibrato, more easily the higher
the sample rate is set.

The real fix is the decimation the entry below already wants for a different
reason. This is a second and larger reason for it: a fixed analysis rate well
above the vibrato band is the one thing that makes the sampling unable to alias
with a vibrato at all, at any device setting. The filter specification is still
the first piece of work.

### What was built

- **`Tuner::Pacing`**: the duty cycle, the dither, and the cost table they were
  measured from. Two lines in `TunerPanel.h` before, where nothing could test
  them, which is what produced fault one.
- **`GateSettings::holdFrames`**, three, with the table above as its whole
  argument. Nearly free on what the gate is for: on a settling pluck the rate
  falls monotonically, so once satisfied it stays satisfied and the cost is two
  more readings. The display opens at 1.70 s instead of 1.50 at ten readings a
  second, and the reading when it opens is better rather than worse, 0.168
  cents sharp instead of 0.284.
- **`GateSettings::stallFrames`**, which throws a window away rather than
  measuring across a gap far longer than the caller's pacing.
- **`Tuner::requiredRingCapacity` and `studioRingCapacity`**, so the ring's
  capacity requirement is arithmetic rather than prose. The two numbers it
  relates used to meet in `AudioEngine.cpp`, where no test can compile them
  together, so halving the constant to a figure that still looked generous
  would have started accepting torn snapshots silently.
- A pass with no new audio no longer re-analyses the buffer it already
  analysed, which mattered once several readings had to agree: two of the three
  could have been re-reads of one buffer.

### What the audit of this work found

A fresh auditor over this session's own commits, per the rule that the auditor
must not be the author, and it earned its keep immediately: **the bound added
for the stall case did nothing at all.**

The panel computed one interval and handed it to both sides of the comparison,
`setFrameSeconds (hop)` and then `process (..., hop)`, against
`gap > stallFrames * frameSeconds`. With both equal to `h` and `stallFrames`
clamped at two or more, the test is `h > 4h`: false for every positive `h`. The
one line meant to protect a window was the line disabling it, and the comment
above it claimed the opposite. Measured by the auditor: four readings on a
steady note, a ten second gap, and two readings later the display opens settled
on a string being turned at five cents a second, reporting the movement as one.
`setFrameSeconds` is called after `process` now, so the bound is scaled by the
previous hop.

Three more from the same audit:

- **The bound was tested against the clamped gap.** The clamp caps a gap at ten
  seconds, so once the bound passed ten, which four times a three second
  nominal does and a loaded 192 kHz machine reaches, no gap of any length could
  cross it. It reads the raw figure now, written `! (gap <= bound)` so a gap of
  NaN counts as a stall rather than as no time at all.
- **`Pacing::expectedHop` was dead code and a trap.** A caller using it and
  then sleeping for `nextWait` takes two draws from the generator and tells the
  gate an interval it never waited. Removed rather than documented.
- **Eleven quoted figures in the new headers were not figures the repository
  produced.** Most came from the wider sweep rather than the one the test runs,
  one was a 192 kHz measurement labelled as 96 kHz, one comment claimed the
  printed output showed the opposite of what it showed, and the ring comment
  still named the constant the same commit had replaced. All corrected against
  the test's own output. This is the fault `CLAUDE.md` names and not a tidiness
  matter: a header with a plausible wrong number in it is how somebody spends a
  night tuning the wrong parameter.

### Mutation testing

**Sixty two deliberate breakages across three runs**, each compiled with the
sanitizers `Tests/run.sh` uses, because a `-O2` harness reports gaps that are
not there and hides the ones that are.

The first run was 34, of which 25 died. Four survivors were real gaps, each
closed and each closure confirmed by re-running the mutation:

- **Replacing the spread across the window with the difference between its
  ends** survived, as it had survived the branch's own audit. It needed a check
  of its own and could not be left to the vibrato sweep: the two criteria are
  identical on any pitch that only goes one way, and every other signal in the
  file is monotone or steady. A vibrato whose period divides the window puts
  both ends on the same phase while the readings between them are twenty cents
  away, which reaches it deterministically.
- **Removing the floor that keeps a wait positive** survived, because nothing
  fed it the one combination that drives the base negative: no duty multiple
  and a reading costing more than the nominal.
- **Tightening the nominal's clamp from ten seconds to one** survived, because
  nothing tested a nominal above a second, which is the 192 kHz case.
- **Lowering the largest-block bound** survived, because the requirement is
  recomputed from the same constant and the check stayed self-consistently
  satisfied. A check whose bound is computed from the constant it is checking
  is not a check, which this file already records from the 10-29 night. Both
  hardware bounds are pinned with their reasons now, which is the only thing a
  test can do about a constant that describes the world.

The two faults the audit found were then written as mutations as well, and both
die: the bound against the clamped gap, and the bound written `>` so NaN slips
through.

**One shape is worth keeping from the third run.** The hold counter is zeroed
in four places, and they are not four independent guards: each is followed by a
push onto an empty history, so the clear that happens while the window is
filling zeroes it again on the next reading. Remove any single one and nothing
changes. Remove the filling one together with either the silence or the stall
one and the counter survives the clear, so the first full window after any
silence or stall already satisfies the hold and the hold becomes one agreement
for every note after the first: only the first note of a session behaves as
documented. A sweep cannot see that, which is why the sweep added for fault two
did not: a sweep measures how often something wrong is displayed, and this
changes how soon something right is. The check that catches it counts the
readings a second note needs. The header now says which of the four clears is
load bearing, so that tidying three away cannot leave the wrong one.

Remaining survivors, each a provable no-op and recorded as one: any single
clear; the silence and stall clears together while the filling one stays; the
clear in `reset`; `stallFrames = 1`, which `configure` clamps to two; and
`holdFrames` clamped to zero rather than one, which is identical behaviour
because a counter is never below zero.

### Deliberately left for later

- **Decimating the tap to a fixed analysis rate.** Wanted for two reasons now,
  and the single thing that would most improve this tuner. The filter
  specification is the first piece of work, as the entry below says.
- **The reading cost table and the tear rate figures are still not reproducible
  from anything checked in.** `CLAUDE.md` names `*Probe.cpp` as the mechanism
  for exactly this and there is no `TunerProbe.cpp`. These figures are load
  bearing: they set the duty cycle, which sets the reading rate, which decides
  the whole vibrato result.
- **Two agreements against three.** The suite's sweep cannot tell them apart,
  4.9 cents and 23 readings against 4.9 and 14. Three comes from the wider
  sweep. If waiting ever matters more than those 9 extra readings, two is
  defensible and the measurement is above.
- **Six readings before anything is displayed is 5.4 s at 192 kHz**, which is
  the reading cost rather than the gate. Decimation fixes this too.

---

## 2026-11-06: the tuner merged

Pull request **#61** is squash merged as `2745fcd`, and the release notes are in
behind it as **#69**. All four checks were green on a head that was up to date
with `main`: the tests job and all three platform builds.

Two things about how it got there are worth keeping.

**It took two sessions and a week of wall clock, because one of them was cut
off twice.** The first was cut off after the arithmetic landed, and the branch
sat as a draft overnight; the second finished the wiring and then stopped
against a spend limit with the audit of its own fixes still running, and
resumed a week later to find later nights had carried the branch forward,
merged `main` into it, and run a second audit over it. Nothing was lost,
because every milestone was pushed. That is the whole reason the "save as you
go" rule is in the prompt, and this is the clearest case of it paying so far.

**The audits cost more than the building did, and were worth it.** Between
them they found an estimator that reported a confident octave down above its
search ceiling, a gate whose accuracy depended on how often the display asked
it for a reading, a constant naming B6 that was below B6 so no sweep ever
tested that note, a display that would hold a dead note for ever after an
interface was unplugged, a modal window that would have taken the transport and
the mixer with it, a thread that would have spun a core at 192 kHz, and a
dozen comments that said more than the code did. Eighty eight deliberate
breakages were run against the test, eleven of which it did not catch at first.
None of the faults was in the arithmetic of the paper. Almost all of them were
in the half nobody had thought to doubt.

### Not released

A release was not cut. The change is user visible and all three platforms
built, so it could have been, but what is waiting is now three nights of
features and several hardening passes, and the standing question at the top of
this file asks for the `release-manager` agent over the whole span rather than
over one change. A session resuming into a spend limit is not the session to
do that. It is the next obvious piece of work on this repository and it is
worth more than another feature.

---

## 2026-11-05: selecting more than one note, and what a group drag must not deform

### Status: merged

Pull request **#67**, squash merged as `6ec29af` on 2026-11-05. All four CI
jobs green on the merged head: `tests`, `ubuntu-latest`, `windows-latest` and
`macos-latest`. The release notes went in with it rather than being written
afterwards.

The merge went through the API first time, which is worth recording next to
the 11-03 entry's warning that it took six attempts and gave six wrong
reasons. Nothing was done differently: `main` still has no branch protection
and no rulesets, so that entry's advice stands, which is to check the refs
before believing a failure.

One note for whoever picks the next piece of this up. The platform builds only
start after the `tests` job finishes, and a new push cancels the whole run, so
pushing again while the builds are in flight leaves the component code
uncompiled on all three platforms with the run still showing green for
`tests`. That happened twice tonight and both times the cancelled jobs read as
"cancelled" rather than as failures, which is easy to mistake for a pass. The
builds that matter are the ones on the head that is being merged.

### What was looked at

The lane this night was the piano roll and the data model. Off limits by open
pull request: `Source/AudioEngine.{h,cpp}`, `Source/MainComponent.{h,cpp}`,
`Source/InputSource.h`, `Source/Tuner.h`, `Source/TunerPanel.h` and
`Tests/run.sh` (#61, the tuner), `Source/Fx/Svf.h` (#55),
`Source/Fx/BellMatch.h` (#66) and `Source/Envelope.h` (#65). `Source/Synth/`
and `Source/Fx/` are being built by hand and were left alone.

That ruled out most of what the backlog has been deferring. A time signature,
the biggest single gap in the studio, wants the engine for the play head and
the metronome accent and so is still waiting for a night when #61 has landed.
Stem export, clip gain and the render bounds question all live in
`renderOffline` or `renderAudioClips`. Punch recording reaches the engine too.

What was left is the thing the 11-03 entry already named as "the next thing
worth doing here": the piano roll edits exactly one note at a time. There is
`dragNote`, singular, and drag modes for moving, resizing and erasing one
note. There is no way to pick up a chord, no way to transpose a riff, no way
to move eight notes a sixteenth later, and no way to quantise part of a
pattern rather than all of it. A musician who builds patterns does those
things constantly, and every studio checked has done them for twenty years.
So this is the third of the three kinds of candidate the brief asks for at
once: missing, standard everywhere else, and a workflow step that takes more
actions here than anywhere else. Eight notes transposed an octave is eight
drags here and one gesture in every other studio.

The 11-03 entry deferred it as "almost entirely untestable UI work". That
turns out to be wrong, and it is worth saying why, because it is the reason
this was buildable tonight. The hit testing and the mouse plumbing are
untestable UI. The part that will be silently wrong is not: it is the
arithmetic of moving a set of notes as a set, and it has exactly the shape
this project keeps getting bitten by. A group drag that clamps each note
individually at the top of the keyboard does not error, does not crash and
does not look broken. It flattens the top of a chord into a unison, and the
person who did it finds out later, by ear, with no idea which gesture did it.
That belongs in a header with no JUCE in it and a randomised test over
thousands of generated selections, and then the component is thin.

### What the research found

Full report from `dsp-researcher`, from the manufacturers' own manuals. Ten
questions were asked. Six have a published answer, four do not, and the four
are recorded below as this project's own choices rather than borrowed
authority, which is what `CLAUDE.md` asks for.

Published, with quotes:

- **A marquee selects by touching, not by containing.** Apple's own general
  selection chapter, Logic Pro 9 User Manual chapter 8: "All elements touched,
  or enclosed, by the selection rectangle (shaded area) are selected." Avid
  says the same of the Grabber tool: "if any portion of the rectangle touches
  a note (either its start or end point), the note is included in the
  selection." No manual anywhere says a note must be fully contained. Two more
  require one point inside: Avid's Selector tool wants the start point, and
  Steinberg's Score editor wants the notehead. Intersection is the only
  convention with explicit support.
- **Shift adds, and the same Shift removes.** Unanimous but for one studio.
  Live 12: "You can add the Shift modifier to add more notes to your current
  selection", and "You can also remove a single note from your selection by
  holding down Shift and clicking on it." Logic 9 for the marquee case: with
  Shift held, "the active selection status of the elements is reversed", which
  is a toggle over the rectangle rather than an add. Nobody documents a second
  modifier for toggling, so none was invented. FL Studio is the exception and
  uses Ctrl+Shift.
- **A group keeps its relative positions.** Steinberg, Moving and Transposing
  Note Events: "All selected note events are moved, maintaining their relative
  positions. Snap is taken into account."
- **At a limit the whole group stops, and breaking the shape is opt in.**
  Apple documents this exactly, for velocity: "If the velocity value of any of
  the selected notes reaches the minimum or maximum value (0 or 127), none of
  the notes can be adjusted further. You can override this behavior by
  pressing and holding Option while dragging", and then, naming the
  consequence rather than hiding it, "By overriding, the velocity relationship
  of this note to the other notes is altered." Ardour corroborates it from the
  other side: its shortcut list carries a separate "(allow mush)" variant of
  every transpose and velocity command, so the plain command does not mush.
- **Velocity on a group is additive, not scaling.** Apple: "When you edit the
  velocity of multiple notes, the relative differences in their velocity
  values are retained." Nobody documents multiplicative scaling as the default
  drag behaviour anywhere. Absolute, every note to one value, is a separate
  documented gesture: Apple's Option-Shift, FL's Shift and drag, Studio One's
  inspector.
- **Length on a group is relative too, with two distinct absolute variants.**
  Apple: "The relative differences between the lengths of the selected notes
  are maintained", then Shift for "the same end point for the selected notes"
  and Option-Shift to "make them all the same length". Those two diverge the
  moment the notes have different starts, and both are useful, so both are
  here. Studio One publishes the same three with different keys.
- **A duplicate lands immediately after the selection and becomes the
  selection.** Studio One: "The duplicated Event is always placed after the
  original Event, and it is automatically selected once duplicated", and
  explicitly not at the playhead. FL Studio's Ctrl+B: "The selection, or all
  notes if nothing is selected, will be duplicated to the right of the
  original selection." The second half matters: the copy becoming the
  selection is what makes three duplicates in a row build a chain rather than
  a pile.
- **A moved note wins over what it lands on.** Live 12: "If the new note
  overlaps with the beginning of the original note, the original note will be
  overwritten. If the new note overlaps with the end of the original, the
  original note will be shortened." The asymmetry is the rule: it turns on
  whether the covered note's start survives.

Not published anywhere, so chosen here and said so:

- **Whether clicking a note that is already selected keeps the selection.**
  Not one of the eight manuals states it, and it is the behaviour a group drag
  depends on entirely. It is forced by documented procedures that are
  otherwise impossible (Apple: "Select the notes, then drag either edge of one
  selected note", with the relative lengths maintained, which needs the other
  notes still to be there), so the selection is preserved. It is also not
  collapsed on mouse up, because a click to audition a note should not destroy
  a selection somebody spent a minute building.
- **Which note anchors the snap on a group drag.** Nobody publishes it. The
  grabbed note, because it is the only choice that puts the snap under the
  mouse, and because the clicked note is the documented reference in every
  other group operation. Earliest-in-selection is published by nobody and
  would make the same drag behave differently depending on which note was
  grabbed.
- **Whether a group drag snaps absolutely or relatively.** Both are named,
  first class modes, and there is no majority: Apple's "Snap Regions to
  Absolute Value", "When you move or edit an item, it snaps exactly to the
  nearest grid position", against "Snap Regions to Relative Value", which
  keeps "the same relative distance from their original grid positions";
  Steinberg's "Grid" against "Grid Relative", where "a moved event keeps its
  original position relative to the grid". Absolute is Apple's and
  Steinberg's default; relative is Ableton's: "Note movements also snap to an
  'offset,' based on each note's original placement relative to the grid."

  **Absolute is the default here**, and the reasoning is worth recording
  because this entry first said the opposite and said it for a reason that
  does not survive reading what absolute actually does to a group. Neither
  mode quantises a performance. In both, one delta moves every note and the
  group keeps its internal timing exactly; the modes differ only in where the
  GRABBED note ends up. Absolute puts it on a grid line and carries the rest
  along by the same amount, relative moves the whole group by a whole number
  of steps and leaves every note at its own distance from the grid. So the
  choice is between placing a group precisely and nudging it without touching
  its relationship to the bar, not between keeping a feel and destroying one.

  Absolute wins the default on two counts relative does not answer: it is
  what dragging a single note in this roll has always done, so no existing
  gesture changes under somebody's hands, and it is what Apple and Steinberg
  both ship. Relative is offered rather than dropped because it is the right
  answer for moving something played and left off the grid on purpose, which
  is the case absolute handles least well. It is on the Selection menu.

  The first version of this entry claimed relative was the default and
  justified it with "moving a played riff a sixteenth later should not also
  quantise it", which is true of both modes. The audit caught the entry still
  saying it after the code had been changed, which is exactly what these
  entries are for.
- **What a group at a pitch limit does on a drag.** Published for velocity by
  Apple, not for pitch by anybody. The same rule is used for both, which is
  the answer the velocity text and Ardour's command names both point at.

### What was rejected, and why

- **Ableton's one dimensional timespan selection model.** Live's structural
  selection is a time range: "Clicking and dragging in the MIDI Note Editor
  selects a timespan", and notes inside it come along. It is a real divergence
  and it is deliberately not followed: every other studio checked selects with
  a two dimensional rectangle, and a pitch axis is half of what a piano roll
  is for. Picking three notes out of a chord is impossible in a timespan.
- **A second modifier for toggling, separate from adding.** Documented by
  nobody. One modifier that does both is the published convention and
  inventing a second would be a key nobody coming from another studio would
  guess.
- **Ardour's select the range between two notes.** The only studio with it,
  and the modifier is not published in any readable form: the Ardour manual
  renders with its modifier key macros empty and the source is behind a
  sign in. Implementing it would mean choosing the key myself and implying
  Ardour's authority for it. Left out rather than half sourced.
- **Multiplicative velocity scaling, and compressing a selection toward the
  drag.** Nobody documents either as a drag behaviour. Scaling also fails to
  round trip through integers, so +20 then -20 would not return a pattern to
  where it started, which is the kind of quiet damage this project is supposed
  to refuse.
- **Steinberg's offline Transpose Setup and its "Keep Notes in Range".** It
  octave folds notes that would leave the range rather than clamping, which is
  worth knowing and is not support for a clamp. It is a dialog over a
  selection, not a drag, so it is a different feature and should not be cited
  for this one.
- **Reaper's behaviour, in all of it.** Deliberately absent. The Reaper user
  guide PDFs truncate before the MIDI editor chapter through the fetch tool
  and the wiki serves a stub, so nothing here is sourced from Reaper. Worth
  flagging for later nights: Reaper is the one studio where marquee and
  selection behaviour is configurable in preferences, so anything remembered
  about it is likely to be somebody's remembered configuration rather than a
  default.
- **Legato, deferred rather than rejected.** It is well specified and cheap
  once this lands: Live 12 extends or shortens each note to the next and "The
  last note gets extended to the end of the loop", and Bitwig's Make Legato
  treats a chord as a unit, which is the detail that stops a chord being
  chained into a scale. It is a named command in its own right rather than
  part of selecting, and Steinberg's own Legato topic could not be retrieved,
  so it wants its own entry. The group length arithmetic built tonight is what
  it will sit on.
- **Keyboard shortcuts, deferred by file lock.** Select all, arrow key nudge,
  arrow key transpose (Up and Down for a semitone, Shift for an octave, which
  is the strongest consensus in the whole research: Steinberg, Bitwig, Live
  and Ardour all publish it and agree) and Delete all need key handling, and
  the roll sets `setWantsKeyboardFocus (false)` so that typing plays notes.
  Routing keys to the focused view is `MainComponent`'s job and that file is
  open in #61. Everything tonight is therefore reachable by mouse, and the
  transpose and nudge amounts are in the header ready for the keys to call.
  That is a real limitation and it is written into the pull request rather
  than left for somebody to discover.

### What was built

`Source/NoteSelection.h`, no JUCE, holding the arithmetic: the marquee test,
the selection algebra, and every group edit as a transform over a note list
and a set of selected indices. `Tests/NoteSelectionTest.cpp` over it. Then the
piano roll grew a Select tool, a marquee, a group drag, a group resize, a
group velocity drag, and a Selection menu for the operations that are not
gestures.

### One rule runs through all of it

Every limit in the header is turned into a limit on the DELTA, once, over the
whole selection, and then one delta moves every note. Pitch, time, velocity
and length all work that way, and all four take their bounds against zero, so
a limit stops an edit and never forces one.

That phrasing was arrived at twice, both times by something failing. Writing
the test found that bounds taken from the group alone make a selection whose
top note sits above the ceiling come out transposed down by however far it is
over, as the answer to a request to move it sideways. Then the audit found the
same shape again in two places that had been left clamping each note on the
way out: a note arriving from a file shorter than a tick made a request to
shorten the group lengthen it, and a velocity arriving below the floor was
lifted on its own while the rest of the group stood still, which breaks the
one thing those modes exist to keep. The invariant is now the same sentence in
four places: a note that was inside its limits stays inside them, a note that
was already outside is never pushed further out, and nothing is clamped per
note on the way out.

### What the audit found

Nine findings, all fixed or recorded. The one that mattered:

**A single click on a note destroyed notes.** A press on a note begins a group
move from the moment the button goes down, and a bare click sends a mouse up
with no drag in between, so overlap resolution ran on a gesture in which
nothing had moved. Clicking a long note deleted a short one underneath it;
clicking the short one cut the long one back to meet it; with eight notes
selected, one click could delete unselected notes at eight pitches. Reachable
in two ordinary Draw gestures, because drawing a note over another deliberately
leaves the overlap. It was undoable, which was the only mercy. Resolution is
now gated on the pattern actually having changed, read off the notes rather
than off the pointer, so a two pixel drag that snaps to no change is a click
as far as the pattern is concerned.

**A selection survived an undo and pointed at different notes.** The first
`validateSelection` only truncated out of range indices, which is a range
check wearing an identity check's comment: an undo rebuilds the clips from
deep copies, so every index can still be in range and every one of them point
at a different note. Transpose then moved notes nobody had picked. The
selected notes themselves are remembered now and anything that does not match
drops the selection, because guessing which note somebody meant is worse than
losing a selection. The channel rack reaches a shared pattern the same way.

**`Move::beatsLimited` was wrong in 42 per cent of clamped moves**, measured.
It tested whether the answer landed near a bound, but a snapped answer sits up
to a whole grid step inside the bound, so real clamps went unreported and
plain snapping was reported as a clamp. It is computed against what the same
request does with the limits removed now, which is the only thing that tells
snapping and clamping apart, and the test pins it both ways.

The rest: the Selection menu captured a raw `this` where the quantise menu
beside it uses a `SafePointer`; `duplicate` tested indices against a list it
was appending to, so a stale index could come alive and the copies start being
copied; duplicating no longer resolves overlaps, because a duplicate is a
create rather than a move and resolving let it shorten one of the notes it had
just copied; the Select tooltip said Command on two platforms that have Ctrl;
and the snap submenu is named for moving, because a group resize snaps its end
absolutely the way the single note resize always did and does not read the
choice.

### Mutation testing

61 mutations in the first pass, 51 caught. Of the ten survivors, three are
genuine no-ops and seven were real gaps in the test, every one of them a
branch the generators could not reach:

- Clamping each note inside `applyMove` is a no-op, which is worth writing
  down because it is the headline fault the header exists to prevent. The
  delta is already guaranteed to keep every in-range member in range, so the
  per note clamp cannot bite. The meaningful versions of that mutation live in
  `planMove` and are killed hard: dropping the pitch ceiling's bound against
  zero fails over twenty thousand checks.
- `lower_bound` to `upper_bound` in `remapAfter` is a no-op, because indices
  present in the erased set are skipped one line above and the two bounds
  agree for every index that reaches it.
- Dropping the correction on the quotient's rounding in `snappedDeltaWithin`
  is very nearly a no-op: measured, it fires zero times in three million
  realistic cases and seven times in five million adversarial ones, with the
  phase pushed to a billion and the grid to two to the minus twenty. It is a
  real guard that nothing reachable from the roll can trip, and it stays
  rather than being deleted because the arithmetic that would have to be
  trusted instead is a rounded quotient.

The seven gaps, all now closed, are the more useful list, because each one is
a property the test claimed and did not have: that a move does not change a
note's length or velocity; that snapping takes the NEAREST step rather than
merely a feasible one; that absolute velocity clamps a value handed in from
outside; that the velocity floor is the one a file can carry, checked through
`midiVelocityToByte` rather than against the constant itself, so setting the
constant to zero now fails; that the two absolute length modes floor what they
are given; that no unselected note is left overlapping a selected one at the
same pitch, which is the post-condition the whole of `resolveOverlaps` exists
to produce and which nothing had asserted; and the two cases its generator
could never produce, two notes sharing a start and two movers inside one
victim.

The generators now plant what they could not reach: zero length notes, lengths
and velocities below their floors, and notes that start together. Each one of
those was a surviving mutation before it was planted.

Second pass after the fixes: 70 mutations, 63 caught. Five of those seven
survivors were real gaps and are closed, and the recheck confirms them caught,
so **68 of 70**. The five were: planting zero length notes did not pin the
branch that handles them, because inside a rectangle the span test and the
point test agree and come apart only at an exact edge, which random doubles
never land on, so the distinguishing cases are written out by hand instead;
the claim that nothing is clamped per note on the way out was untested for
time and pitch, because a per note clamp can only be seen through a note
already outside the clamp's own range and every note the generator made was
inside it; the velocity floor was checked through `midiVelocityToByte`, which
clamps, so the check could never fail and setting the floor to zero passed;
and a group velocity edit was not required to be monotonic, so a bound not
taken against zero went unnoticed.

The last of those is the lesson worth carrying: a check written against the
header's own constant cannot catch the constant being wrong. The floor is
checked against `midiVelocityFromByte (1)` now, which is `MidiTimebase.h`'s
own figure computed without reference to this header at all.

---

## 2026-11-03: quantise, and what a strength percentage is a percentage of

### Status: merged

Pull request **#64**, squash merged as `7ae88d9` on 2026-11-04. The `tests`
job, `ubuntu-latest` and `windows-latest` were green on the head;
`macos-latest` was still building, which is routine here and has not failed.
The release notes went in with it rather than being written afterwards.

Built across two nights: 11-03 did the research, the header, the test, the
mutation pass, the piano roll and the first two audit rounds, and was cut off
before finishing. 11-04 picked the draft up, ran the third audit round, fixed
what it found, wrote the notes and merged.

One note for whoever reads this next to the API rather than the web interface.
The merge took six attempts and the reasons GitHub gave were all wrong: the
pull request's record was stuck with `mergeable: null` and `mergeable_state:
"unknown"` for twenty minutes and never settled, and the endpoint refused with
"head branch is out of date" when the branch was strictly ahead of `main` and
`main` was the merge base, "head branch was modified" when the ref had not
moved for minutes, then 500 and finally 502. The 502 was returned **after the
merge had already been performed**, so the state to trust is the pull request
itself and the ref, not the response. There was never a review gate: `main`
has no branch protection and no rulesets. An earlier version of this entry
said the session was not permitted to merge, by analogy with #59, and that was
wrong. Check the refs before believing a failure here.

### What was looked at

The lane this night was editing and the data model: the engine and the window
were both off limits, because pull request #61 (the tuner) has
`AudioEngine.{h,cpp}`, `MainComponent.{h,cpp}` and `InputSource.h` open, and
`Source/Fx` is not clear either because #55 has `Svf.h` open. So anything
whose work is in the audio path waited.

| Candidate | Verdict |
|---|---|
| **Destructive note quantise** | **Chosen.** The studio has no way to move a played note onto the grid. `Groove.h` is the opposite job: it swings notes that are already on the grid, at playback, and leaves anything played off the grid where the performance put it. So a part played in live cannot be tidied at all, and that is one keystroke in every other studio. |
| A time signature | Deferred, and it is the biggest single gap found tonight. The studio is 4/4 everywhere: `SongLength.h` has `kBeatsPerBar = 4.0` as a constant, `MidiFileIO` writes a fixed 4/4 meta event, and the play head handed to plugins is a default-constructed `juce::AudioPlayHead::TimeSignature`. Nothing stores a numerator at all. Fixing it reaches the ruler, the metronome's accent, the loop end, the play head and the file format at once, and two of those files are in #61. It wants a night where the engine is free. |
| Multi-note selection in the piano roll | Deferred. The roll has draw and erase and no selection, so notes are edited one at a time, and that is worse than it sounds: there is no way to move or transpose eight notes together. But it is almost entirely untestable UI work, and quantise is the thing that needs the selection least, since every studio applies it to the whole clip when nothing is selected. Worth a night, and the quantiser built tonight takes a plain vector so it will slot straight in. |
| Stem export | Deferred: the render loop is `AudioEngine::renderOffline`, which #61 has open. |
| Clip gain on audio clips | Deferred for the same reason: it is read in `renderAudioClips`. |
| Destructive randomise, as part of quantise | **Rejected, deliberately.** Avid publishes a quantified definition and it would have been cheap, but `Groove.h` already humanises at playback, where it can be dialled while listening and turned off without having lost anything. A second, destructive humaniser would be two controls meaning nearly the same thing, and the destructive one is the worse of the two. |
| Logic's Q-Length and Q-Velocity | **Rejected**, and worth recording as a near miss. They read like "quantise the note end" and "quantise the velocity" and they are neither: both are percentages toward a **template region's** lengths and velocities, which is Logic's groove template feature. Building them under those names would have shipped Apple's name on a different feature. |
| Logic's Q-Flam | Rejected for tonight: it spreads the notes of a chord into an arpeggio, which is an arrangement idea rather than a timing fix, and Apple publishes no unit or range for it. |

### What was chosen

`Source/Quantise.h`: destructive quantise of stored notes, as an editing
action on a MIDI clip, with the grid, a strength, swing, a range gate, and a
choice of what moves. No JUCE, so all of it is tested.

### Where the behaviour came from

The `dsp-researcher` agent went to the manufacturers' own manuals. The single
most important finding is a negative one: **no manufacturer publishes an
equation for quantise.** Four publish the behaviour in words, and two open
source projects publish a numeric definition that agrees with them. So the
header says in its own comment which parts are quoted and which are this
project's decision, rather than implying an authority that is not there.

**Strength** is linear interpolation between where the note was played and the
grid, `new = old + strength * (grid - old)`. Nobody writes that equation, and
four sources describe exactly it:

- Avid, Pro Tools help, Additional Grid Quantize Options: notes are "moved a
  percentage toward the Quantize Grid", range 0 to 100%.
- Apple, Logic Pro User Guide, Quantize parameters (lgcp35029a2b): Q-Strength
  "determines how far a note or transient marker is shifted toward the nearest
  grid position", "100% results in full quantization, and 0% leaves the note
  ... at its recorded position".
- Steinberg, Cubase Plug-in Reference, Quantizer: Strength "determines how
  close the notes should be moved to the quantize grid", and at 100% "all
  notes are forced to the closest grid position".
- Rosegarden's manual is the most explicit of any source: each note "will only
  be moved that percentage of the distance toward its nearest grid line".
  Ardour's says "as a percentage of the nominal distance".

Ableton is the one that does not fit: Live's manual says Amount moves notes
"only by a percentage of the set quantization value", which read literally is
a percentage of the **step** rather than of the distance, and would overshoot
the grid. Taken as loose prose for the same thing, since the literal reading
is not a quantiser.

Steinberg's iterative quantize page states a property that falls out of
interpolation for free and is the best single discriminator against a "snap if
close enough" shortcut: "Repeated use moves them progressively closer to the
grid." Applying strength `s` twice leaves exactly `(1 - s)^2` of the original
offset, which is smaller than `(1 - s)` and never zero below 1. The test
asserts it.

**Swing** is the position of the offbeat within its pair, 0.5 straight,
0.6667 the third triplet, which is the convention `Groove.h` already uses, so
an edit and a playback setting cannot disagree about what a swing number
means. Apple's Q-Swing is on that scale and is quoted for it: it "alters the
position of every second point in the current quantization grid", "values over
50% delay the beats; values under 50% pre-delay (advance) them". Apple's own
named presets give six exact points on the scale, 50, 54, 58, 62, 66 and 71
per cent, and those six are in the test as fixed cases.

Pro Tools and Rosegarden use the other published scale, 0% straight and 100%
a triplet feel, with Avid's maximum of 300% putting the offbeat on the next
grid point. The two convert exactly, `p = 50 + S/6`, pinned at three published
points rather than one, and the header says so, because somebody reading an
Avid number into this control would otherwise be four times out. Cubase's
swing is a percentage whose numeric meaning Steinberg never states, so it
cannot be matched from the documentation, and the header says that too.

**The range gate** is the one every manual warns about by the way it is built,
and Avid is the only source that quantifies it. Apple's Q-Range uses a sign:
"a value of 0 means that every note ... is quantized", and "negative Q-Range
values move only notes ... that fall outside the set range", leaving the ones
near the grid alone. Apple documents only the negative case. Avid splits the
same thing into two separately named options so the sign cannot be misread:
**Include Within**, "only quantized if located within the specified percentage
of the Quantize Grid", and **Exclude Within**, "not quantized if located
within". Cubase has the same pair as Catch Range and Non-Quantize.

This header follows Avid and names the two rather than signing one, because
the sign is the documented trap: implemented backwards it is a control that
visibly does something, to the wrong half of the notes. The window's width
comes from Avid's two worked examples, which are the only quantified
definition of a quantise gate in any primary source: a quarter note grid with
Include at 50% gives "an eighth note area around each beat", and at 25% gives
"a sixteenth note area", so the window is `range * grid` wide, centred, and a
tolerance of `range * grid / 2` either side. At 100% the window is a whole
step wide, which on a straight grid reaches every note, because nothing there
is further than half a step from a grid point. Under swing it does not: the
gap from a downbeat to a swung offbeat is up to one and a half steps, so a note
in the middle of that gap is three quarters of a step from either end and
stays outside even a full window. That is what Avid's definition says rather
than something to correct, and the test pins it.

**What moves** follows Avid's rule, which is the clearest published statement
of the note length question and the one the other manuals agree with
structurally. Note On and Note Off are separate choices; "Preserve Note
Duration" is **unavailable** when both are selected, because the two requests
contradict each other and Avid removes the option rather than silently picking
one. Cubase splits the job into three differently named commands, Quantize
("quantizes audio or MIDI event starts"), Quantize MIDI Event Ends and
Quantize MIDI Event Lengths, rather than one command with a checkbox. Ableton
says what end quantise costs: it "will stretch the note". Logic's Smart
Quantize is described by what it refuses to break, keeping "the relative
position of note on, note off, and CC events".

So: starts only, length carried along, is the default, and anything else is an
explicit choice. `preserveLengthApplies` returns false when both ends are
being quantised, and the test asserts that the request is refused rather than
quietly ignored, which is Avid's design copied deliberately.

### What is a decision rather than a standard, stated as one

- **Collisions, and only the ones quantise caused.** No manufacturer documents
  what happens when two notes of the same pitch land on top of each other, and
  quantise is the operation that causes it, since two notes a sixteenth apart
  quantised to a quarter grid become one. This project already has a rule, in
  `MidiTimebase.h`, written for the MIDI file round trip: the earlier note is
  shortened to where the later one starts, and of two that begin together the
  longer is kept and the other is counted as dropped. Quantise follows it, so
  a quantised pattern exports as itself, which is what the cross-check in the
  test measures.
  **A pair that already overlapped is left alone**, and that restriction is
  the whole of it: a held note re-struck before it is released is ordinary
  material, and a pass that tidied one would be deleting part of a performance
  nobody asked it to touch. The first version asked a weaker question, whether
  either note had moved, and the audit showed what that costs. See the audit
  section below.
- **The minimum length** is one export tick, 1/960 of a beat, from
  `MidiTimebase.h`'s existing rule that "a note on and a note off at the same
  tick is a note that some studios draw and none of them play".
- **Swing is clamped to 0.25 .. 0.75.** The top of that is Apple's own:
  "The most practical settings are between 50% and 75%, which give a swing
  feel to strictly quantized (or tightly played) regions." The bottom is this
  project's, made symmetric so the pre-delay Apple documents has the same room
  as the delay. It also keeps the grid usable: at 0 or 1 the offbeat lands on
  a downbeat, the grid has half as many distinct points, and "the nearest grid
  point" becomes a coin toss between two identical positions. Apple's whole
  preset table, 50 to 71, is inside it.
- **Pairs are counted from the clip's own origin**, which is what `Groove.h`
  already does. No manufacturer says whether swing pairs count from the bar,
  the clip or the project, so the two features here agree with each other
  instead.
- **Automation lanes are left alone.** Cubase has a MIDI CC option that moves
  controllers with their notes, which needs a note-to-controller association
  this project does not have: its lanes are curves rather than events
  belonging to a note. Moving a curve under a quantised note would be
  guesswork.

### What the mutation pass and the audit found

Both are worth recording, because the two faults that mattered were both the
same shape: a rule that was *nearly* the right rule, with a comment claiming
the right one.

**Mutation testing**: 41 deliberate breakages compiled with the sanitizers,
21 caught by the test as first written, 9 real gaps closed, 6 established as
no-ops by an equivalence dump rather than by argument. The gaps worth naming
are the gate's boundary being made strict, which leaves a note sitting exactly
on the window's edge outside it (against a quarter note grid with Avid's fifty
per cent window that is every eighth in a programmed part), and swapping the
drop and trim branches, which deletes the later note instead of shortening the
earlier one and still leaves a pattern a file can hold.

**The audit** found two faults that would have lost somebody's work.

The first is the one above: **a pre-existing overlap was resolved whenever
either of its two notes moved at all.** A held eight beat note whose start was
a thousandth of a beat off the grid lost half its length, and in the worst
version the note that lost its tail had not moved at all. Sustained piano, a
re-triggered pad and a roll on one drum all produce exactly that material. The
fix is to ask whether the pair overlapped before the pass rather than whether
either note moved, which needs the before picture and so changed
`resolveOverlaps`'s signature. It also removed a cascade: under the old rule a
gated pass could take one more pass per note to settle, twenty five for a line
of twenty four, because each trim brought another end inside the gate's
window. That case is kept in the test as a regression.

The second: **quantise could push a note past its clip's own end**, where the
engine and the export both skip it, so it sits in the roll looking normal and
makes no sound. One bar of sixteenths quantised to a beat grid left five notes
of which one was silent.

The first fix for that grew the clip to cover the note, copying what drawing
and dragging do, and a second round of the audit showed why that is wrong: a
clip is a window onto its pattern and is often deliberately shorter than it,
which is how one bar of a longer pattern gets looped. Growing it expanded a
one bar clip to eight bars, un-hid every note the trim was hiding and moved
over whatever was next on the track, on any press, which is far easier to hit
than the silent note it was fixing. So the window is a quantise option now,
and a note whose onset is inside it is never moved out of it: it stays where
it was played, which is visible, instead of becoming silent, which is not. The
same rule covers the front edge, which the second round also showed is
reachable in two gestures after a clip's left edge has been dragged, and which
the growing fix could not have helped with at all.

The second round found one more fault of the same family, and it is the reason
to run an audit twice. Resolving only self-caused collisions by comparing each
note with the one before it is not enough: an inherited overlap is left in
place, so it can sit between two notes the pass has just brought into
collision, and that pair is then never compared. Half a per cent of passes
over ordinary material came out with an overlap the pass had created, and an
overlap of one pitch plays differently from the way it bounces, because the
engine sends one note off per pitch while the export resolves the pair. Every
note of a pitch already kept is compared now, not just the last one. The
missing assertion, which is the one the invariant always needed, is that no
pair may overlap after a pass that did not overlap before it.

Also fixed from the audit, none of them data loss: the counts reported work
that had been attempted rather than done, so a pass on a settled pattern
claimed to have moved notes it had left alone; a position far enough out
overflowed the signed arithmetic the grid search counts pairs with, which is
undefined behaviour rather than a wrong answer; the tuplet formula in the
comment was the reciprocal of the one Avid publishes, in the sentence directly
after quoting Avid's own worked answer; the claim that a full window reaches
everything is only true on a straight grid; the tooltip called a note lost to
a collision "merged", which sounds like nothing was lost; and the last pass's
report was not cleared when another clip or another project was opened, which
is the leak `CLAUDE.md` says is the one that survives testing.

Two claims in the header were also wrong rather than the code: the convergence
figure cited a measurement that was not in the test, and Apple's swing preset
table was quoted from research the audit could not reach. Both were checked
again: the measurement is now in the test and prints its own worst case, and
Apple's table was re-read at two of Apple's own pages, which give 50, 54, 58,
62, 66 and 71 and no 75.

### A third audit round, on the night of 2026-11-04

The branch was picked up unfinished and the remaining work was the audit and
the notes. A third round was run rather than trusting the second, for one
specific reason: the second round found two faults **in the first round's own
fixes**, and its own fixes had then never been read by anybody who had not
just written them. That reasoning paid for itself.

**It found one more data loss fault, and it was in the second round's fix.**
The sub-tick merge, which turns two onsets closer than an export tick into
one note, dropped whichever of the pair was sorted later. The sort puts the
longer note first only where two starts are **exactly** equal, and that
branch fires on starts that are unequal and under a tick apart, so the
length had no say in it at all. The second round widened the branch to fire
on pairs that were already overlapping, which is what made it reachable from
ordinary material.

What it cost, measured rather than imagined: a sixty-fourth of a beat
artefact at velocity 0.10 struck a tick and a half before a two beat bass
note at 0.95, quantised to the beat at the eighty per cent the piano roll
offers as the setting that keeps the feel. The artefact is drawn to within a
third of a tick of the bass note, the two become one onset, and the pass
deleted the bass note and kept the artefact. A double triggered pad, a noisy
input or a controller sending a stray note on all produce exactly that.

It was also a disagreement between two code paths over the same material,
which is the fault shape `CLAUDE.md` says is the worst this studio can have.
`MidiTimebase.h` resolves the identical pair by keeping the longer, so the
destructive edit and the bounce disagreed about which of the two was the
note, and the edit was the one throwing the performance away. The header had
already promised the project's own rule, "of two beginning together the
longer is kept", in the paragraph directly above the code that did not do
it: a comment claiming the opposite of the code, for the third time in this
repository's history.

The rule is now the export's rule, the longer of the two at its own onset,
and of two equally long the earlier.

**And it found that the assertion the second round credits with catching its
own fault has almost no power.** The check that no pass may create an
overlap of one pitch asked whether *any* pair in the before picture
overlapped, not whether the pair in hand did, while the comment directly
above it described matching an output pair back to the input pair it came
from. Measured: 19749 of its 20000 rounds contain some inherited overlap, so
it passed without looking at the pair in question 98.7 per cent of the time.
Restoring the neighbours-only scan it was written to catch produced **zero**
failures from the randomised sweep; only the one hand-picked three note case
failed. It could not tell the fixed code from the broken code.

Each note now carries a tag in its velocity, which `apply` leaves untouched
and which compares exactly at powers of two over 128, so an overlapping
output pair is matched back to the exact pair it came from. The same mutation
now fails it 1390 times, which is the figure the second round's entry claimed
and did not have a check for.

Two lessons worth keeping, both about this project's own habits rather than
about quantise:

- **The entry below claimed two measurements that were not in the test.** The
  second round removed an earlier pair of unchecked figures for exactly that
  reason and then introduced another one in the same commit. A figure in a
  commit message or a backlog entry is not a check, and the only cure that
  has worked here is putting the measurement in the test and letting it print
  its own worst case.
- **An audit round that fixes something needs an audit round after it.** Two
  rounds in a row found their predecessor's fix wrong, in the same part of
  the same function. Rounds one and two each found faults that were
  introduced by the round before, which says the stopping rule should be a
  clean round and not a fixed number of them. This third round's own fixes
  were mutation tested, eight mutations with the sanitizers and seven caught,
  with the survivor established as a true no-op by an ordering argument
  written into the header rather than by assertion.

Also from this round, none of it behaviour: the claim that quantise leaves a
pattern the export never has to repair had one uncovered case, a pair struck
within a tick before the pass where one of the two is itself shorter than a
tick, which nothing in the studio can make and which the header now states
and sources instead of implying away; `preserveLengthApplies` is false for
two targets for two different reasons and the comment gave only Avid's; and
the tooltip rounded swing to whole per cent, so the menu item labelled
66.67% reported back as 67%, a setting nothing in the menu can reach.

### Still to do

A keyboard shortcut for it, which
needs `MainComponent`'s key handling and so wants a night when that file is
not open in somebody else's pull request. Quantising a selection rather than
the whole clip wants a selection to exist in the piano roll first, which is
its own night's work and is listed above as the next thing worth doing here.

---

## 2026-10-29: whether the loudness histogram's ceiling should move

Handed on rather than taken, because it changes a design constant and costs
work in the device callback, and the night that found it was a hardening run.

`LoudnessMeter.h` stores its block distribution as a histogram from -70 to +30
LUFS at a hundredth of an LU per bin, which is 10000 bins and about 160 KB. The
ceiling was chosen because +30 LUFS is thirty decibels past anything a
converter can pass, so nothing can reach it without a gain mistake upstream.
The night of 2026-10-29 fixed the part of that which was a fault: a programme
above +29.995 LUFS used to report no integrated figure at all, because the
relative gate landed in the catch all top bin and the rule that decides the
gate's own bin by its centre then discarded the only bin with anything in it.

What is left is a degradation rather than a fault. Above the ceiling every
block shares one bin, so the relative gate cannot separate them and the figure
is the ungated mean of everything up there. Measured: 5 s at +60 then 50 s at
+31 reads +49.540 where the exact gated figure is +59.875, and over four
hundred thousand randomised programmes the worst case is 9.5 LU low. The
loudness range collapses to 0.0 LU up there for the same reason, which the mix
report prints in its warning colour.

The question is whether that is worth removing. Raising `highestLufs` to +150
makes the measured case exact: it was tried and it reads +59.875. The costs are
that `numBins` goes from 10000 to 22000, so the histogram grows from about
160 KB to about 350 KB, which is nothing, and that `updateIntegrated` scans
`numBins - from` bins once per 100 ms block in the device callback, which
roughly doubles. That is still a small number against a 100 ms budget, but it
is audio thread work added to buy accuracy in a band that only a mistake
reaches.

Three things worth weighing, written down so the decision is made on them
rather than remade from scratch:

- Any finite histogram has a top, so raising it narrows the band this happens
  in rather than removing it. +150 LUFS is 150 dB past full scale, which is
  past anything a float mixer reaches without something diverging.
- The reading up there is already finite and already says the master is tens of
  decibels too loud, which is the only thing anybody needs from a meter at that
  point. Getting it exact buys precision in a reading nobody will act on
  precisely.
- Against that: the ceiling's original justification was that nothing could
  reach it, and that reasoning is what produced the fault. A constant chosen
  because a case is unreachable is the shape of thing this project keeps having
  to fix.

Not urgent either way. The fault is fixed and the limit is documented in the
header and pinned by a test that requires the figure to stay finite and above
+30.

---

## 2026-10-28 and 2026-10-29: a tuner, and the pitch detector it is built on

### Status: built, tested, mutation tested, audited

The night of 10-28 was cut off after the arithmetic and its test had landed.
The night of 10-29 finished it rather than starting anything new: the engine's
input tap, the window and the readout, then a mutation run and an audit, both
of which found real work. What they found and what was done about it is at the
end of this entry.

### What was looked at

| Candidate | Verdict |
|---|---|
| **A tuner** | **Chosen.** Deferred on 10-21, 10-22 and 10-27, every time with the same one line reason: "still wants a published pitch detector and a window". That is a research problem rather than a design problem, which makes it exactly the thing to send a researcher at, and the answer came back complete. The owner records guitar, every studio and every pedal ships a tuner, and the studio has none, so tuning means reaching for something that is not this studio. |
| MIDI record alignment | Deferred a seventh time, same reason: the figure is easy and knowing whether the keyboard is being played live or a pattern is being bounced off the grid is not. |
| Punch in and punch out recording | Deferred, same reason as 10-21, 10-22 and 10-27. Still wants a published answer to what happens to the audio already on the track, and still reaches `Project`, `ProjectIO`, `CompModel`, the playlist and the engine at once. |
| Noise shaping on export | Deferred, not re-rejected. Still "worth a night of its own, from the paper itself, with the filter designed at the running rate". |

### What was chosen, and why

YIN, implemented from the paper, with the note and cents arithmetic over it and
a gate in front of the display.

The reason for choosing a time domain estimator rather than looking for the
tallest peak in a spectrum is the one fault that matters most here, and it is
sourced. A plucked string's fundamental can be weak or missing and the pitch
is still at the fundamental: Wolfe, "Musical sounds and musical signals",
"If the fundamental is removed, the combined wave still has a period 1/f1, and
still has the same pitch." A spectral peak picker reads the loudest partial
and lands an octave up. The difference function dips at the period whether or
not there is energy at that frequency. Worse, the McLeod thesis records that
for complex tones below 200 Hz "the pitch is mainly determined by the fourth
and fifth harmonics", and every note on a bass and the bottom four strings of
a guitar is in that band.

### Where the behaviour came from

Alain de Cheveigne and Hideki Kawahara, "YIN, a fundamental frequency
estimator for speech and music", JASA 111(4), April 2002, pp. 1917-1930,
DOI 10.1121/1.1458024. The JASA original is paywalled; a full text copy was
read at cagnazzo.wp.imt.fr, whose text layer has OCR damage, so every equation
and every constant below was obtained twice in independent requests and
cross-checked. One disagreement was found and is recorded under the traps.

The six steps, as the paper defines them:

- Eq. (6), the difference function: `d_t(tau) = sum_{j=1..W} (x_j - x_{j+tau})^2`.
  Gross error rate 1.95 per cent, against 10.0 for the plain autocorrelation
  of step 1.
- Eq. (8), the cumulative mean normalised difference function: `d'_t(0) = 1`,
  and otherwise `d_t(tau)` over the running mean of `d_t(1..tau)`. The paper
  says it "starts at 1 rather than 0". Error rate 1.69 per cent.
- The absolute threshold, **0.1**: "choose the smallest value of tau that gives
  a minimum of d' deeper than that threshold. If none is found, the global
  minimum is chosen instead." Error rate 0.78 per cent.
- Parabolic interpolation over each local minimum and its two immediate
  neighbours. Error rate 0.77 per cent.
- The best local estimate, which takes it to 0.5 per cent and is not built
  here. It is a time continuity heuristic for a speech corpus where F0 moves
  continuously, and a tuner has a better conditioned version of the same idea
  available, because the note it is looking at is meant to be steady. The
  stability gate below is that version.

### The two corrections the research made to the premise it was given

Both worth recording, because either one taken the wrong way round leaves
somebody tuning the parameter that cannot fix what they are looking at.

- **The cumulative mean normalisation guards against reading too high, not too
  low.** The paper: replacing d by d' "reduces 'too high' errors", and lets you
  "do away with the upper frequency limit of the search range". The mechanism,
  from the authors' own later paper, is that it "avoids 'too high' F0
  estimation errors that arise if the algorithm incorrectly chooses the
  zero-lag dip", which `d'_t(0) = 1` is what prevents.
- **The guard against reading an octave down is the absolute threshold with the
  first dip rule, and nothing else.** The paper's motivation for that step is
  the case where "one of the higher-order dips of the difference function is
  deeper than the period dip", and the higher order dips sit at multiples of
  the period. So the rule is the smallest lag below the threshold, not the
  deepest dip, and an implementation that takes the global minimum passes every
  obvious test and reports an octave down on exactly the signal a tuner sees.

### The one detail in step 5 that decides whether it was implemented or read

The paper is explicit that the interpolation's two outputs come from two
different functions, and this is the part that gets dropped. Quoting: "the
ordinate of the interpolated minimum is used in the dip-selection process",
and then "the estimate obtained in this way is slightly biased. To avoid this
bias, the abscissa of the corresponding minimum of the raw difference function
d(t) is used instead."

| quantity | taken from |
|---|---|
| the depth, for the threshold test | the interpolated minimum of `d'` |
| the lag, for the period | the interpolated minimum of the raw `d` |

The paper reports the bias and does not state its cause, so the comment in the
header says that rather than inventing a mechanism.

### Why the interpolation is the whole accuracy budget rather than a refinement

Computed from the equal tempered frequencies, and the reason the test tolerance
is what it is:

| note | sample rate | period, samples | one cent, in samples of lag |
|---|---|---|---|
| B0 30.8677 Hz | 44100 | 1428.68 | 0.825 |
| B0 30.8677 Hz | 48000 | 1555.02 | 0.898 |
| E2 82.4069 Hz | 44100 | 535.15 | 0.309 |
| E2 82.4069 Hz | 48000 | 582.48 | 0.336 |

Without sub-sample interpolation the lag quantisation alone is 1.2 cents at the
bottom of the range and 3.2 cents at guitar low E, which is worse than every
shipping tuner. With it, the interpolator only has to be good to about a third
of a sample, which a three point parabola comfortably is.

### The window, which is the half of the question that had been deferred

The buffer the estimator has to hold is `W + tau_max`, not `W`, because
`d_t(tau)` at the longest lag reads `x` up to `t + W + tau_max`. The paper's
own `W = T_max = 25 ms` is that: two periods of the lowest note. Three sources
agree on the floor and they reconcile exactly:

- The paper: "the integration window must be no shorter than the largest
  expected period", and "a larger window leads to fewer errors at the expense
  of reduced temporal resolution".
- de Cheveigne and Baskind, Eurospeech 2003: "time resolution is optimal (one
  can't go below 2T)".
- Boersma 1993: at least three periods of the minimum pitch per window.

Allocating only `W` and computing `d` out to `tau_max` is an off-by-`tau_max`
read past the end of the buffer. This project has already shipped the
analogous fault once, in a delay line that read one sample past its buffer, so
the bound is asserted rather than assumed.

The range **searched** runs from 23.1247 Hz to C8, and the range the one cent
tier is **claimed** over runs from B0, 30.8677 Hz, to B6. Those are four
numbers rather than two, and each of the two gaps is there because of a
measured failure rather than for headroom.

The floor is five semitones below B0, which is Korg's own design target: the
GA-50 manual gives its bass range as B0 to B5 and prints the low end as
23.12 Hz, being B0 taken down the five semitones its flat tuning feature
offers (30.8677 times 2^(-5/12) is 23.1247, matching Korg's figure). Measured,
with the floor at B0 exactly, a B0 that is **one cent flat** already cannot be
read, because its period is already longer than the longest lag computed: the
reading saturates at the floor and says nearly in tune while the string is
out. So the floor is what lets the tuner read the lowest note it covers at the
only moment anybody looks at it.

The ceiling is C8, the top of the detection range Boss print for the TU-3 and
Korg for the CA-50. It is above B6 because a note above the ceiling has no
period inside the lags searched, so the first dip that is inside the range is
the one at twice the period, and the estimator reports an exact octave down
with the threshold satisfied and nothing to say it is wrong. Measured with the
ceiling at B6, a 2.5 kHz tone read 1250 Hz as a confident reading, which a
pinch harmonic or a whistle would produce. Moving the ceiling to C8 does not
remove that, it moves it above the top note of a piano.

`W = 2 tau_max` gives Boersma's three periods in the buffer, which is 130 ms at
44.1 kHz. Latency is free in a tuner and every source says a longer window
means fewer errors.

### The reference pitch, and what shipping products offer

ISO 16, "Acoustics: Standard tuning frequency", specifies A = 440 Hz and was
last confirmed in 2022. Its full text is paywalled; two catalogue entries agree
on the value and on the half hertz tolerance it allows, which is itself about
2 cents at A4.

| product | range | step | source |
|---|---|---|---|
| Logic Pro Tuner | 410 to 470 Hz | 0.1 Hz | Apple's own help page |
| Korg CA-50 | 410 to 480 Hz | 1 Hz | Korg's specification page |
| Peterson StroboStomp HD | 390 to 490 Hz | 0.1 Hz | Peterson's own manual |
| Boss TU-3 | 436 to 445 Hz | 1 Hz | Roland's own manual |
| Korg Pitchblack X | 436 to 445 Hz | 1 Hz | Korg's specification page |

410 to 480 in 0.1 Hz steps is taken: the widest range among the pure tuners and
the finest step among all of them. There is no standard for the adjustable
range, only for the 440, and the header says so.

One discrepancy found and reported rather than resolved: the Boss TU-3 manual's
specification table says 436 to 445 while its own body text says 438 to 445.
The table is treated as primary, and the TU-3W table independently agrees with
it.

### What accuracy is worth claiming

| product | claimed accuracy |
|---|---|
| Boss TU-3 and TU-3W | plus or minus 1 cent |
| Korg CA-50 | plus or minus 1 cent, specified on a sine wave |
| Korg GA-1 | plus or minus 1 cent or better, for guitar and bass sounds |
| Korg Pitchblack X | plus or minus 0.1 cent, in strobe display mode only |
| Peterson StroboStomp HD | 0.1 cent, true strobe |

So the industry answer is two tiers, 1 cent for a chromatic tuner and 0.1 for a
strobe, and the honest part of those specifications is the qualifications: a
sine wave, or strobe mode only. Neither claims the figure on an arbitrary
instrument signal. The test asserts better than 1 cent on synthetic signals
whose answer is known exactly, and deliberately asserts no cents figure on
anything resembling a recorded note, for the reason in the next section.

Perceptual anchor, from the McLeod thesis: "humans can hear a pitch change of 8
cents in a pure tone". One cent is well inside that.

### The trap that decides whether this is right or merely present

**A plucked steel string is sharp when it is struck and falls by twelve times a
tuner's entire error budget while it rings.** Hodgkinson, Timoney and
Lazzarini, DAFx-10, fit the fundamental of an acoustic guitar's open low E to
`f0(t) = (f0_0 - f0_inf) exp(-t/tau) + f0_inf` and measured
**f0_0 = 83.4 Hz, f0_inf = 82.8 Hz, tau = 0.38 s**. That is a fall of
**12.5 cents** with a 0.38 second time constant, and 0.6 cents of it is still
left a second after the pluck. The mechanism is tension modulation: transverse
vibration raises the tension and so the pitch.

So any test that plucks a string and asserts a cents figure without saying when
it reads is measuring the string rather than the estimator, and a tuner with no
gate in front of it reads sharp on every note and disagrees with the pedal on
the floor.

No manufacturer documents what to do about it. The Boss, Korg, Peterson, Apple
and Cockos manuals were all read and none states a level gate, a hold time or a
stability criterion. The only acknowledgements are indirect: Peterson tells the
player to pluck with a thumb rather than a plectrum, and that "it's OK if the
strobe image doesn't completely stop"; Korg specifies one tuner's range on a
sine wave and another's "for guitar and bass sounds"; Apple says only "play a
single note". So `CLAUDE.md`'s rule for where no standard exists applies: the
gate is a decision, it is written down as one, and the arithmetic behind each
figure is in the header.

What the gate is made of, and why each part is there rather than invented:

- **The periodicity test comes free from the paper.** The threshold is readable
  as "the proportion of aperiodic power" the estimator will tolerate, so during
  an attack nothing falls below 0.1 and the estimator is in its global minimum
  fallback. That fallback exists so the estimator always returns something; a
  tuner does not have to show it. Nothing below the threshold is therefore
  reported as "not a note yet" rather than as a number.
- **A rate limit on how fast the pitch is still moving**, which is the tuner's
  better conditioned substitute for the step the paper adds and this does not.
- **A level floor**, because an open string ringing down ends in noise. Boersma
  has a silence threshold for the same job, at 0.05 in his worked example, but
  that is a speech analysis constant and is not presented here as a tuner one.

The criterion is a **rate**, in cents per second, and that is the single
biggest thing the audit changed. The first version asked that the last four
readings lie within a quarter of a cent of their mean, which is the obvious
thing to write and has no notion of time: the spread across four frames
depends entirely on how often the display asks. The audit measured what that
cost by varying nothing but the frame rate, on this very glide:

| frames per second | gate opens at | sharp when it opens |
|---|---|---|
| 10 | 1.40 s | +0.37 cents |
| 15 | 1.23 s | +0.57 cents |
| 20 | 1.05 s | +0.93 cents |
| 30 | 0.86 s | +1.50 cents |
| 60 | 0.56 s | +3.32 cents |

So the whole design, built around the one cent tier, came apart on a choice of
timer interval that nobody had been told was load bearing, and a readout on an
ordinary thirty hertz timer would have shipped a tuner reading one and a half
cents sharp on every plucked note. That is the fault the gate exists to
prevent, arriving through the gate itself, and the test could not see it
because it fixed the frame interval at a tenth of a second.

Measuring the rate instead fixes it, and the arithmetic is simpler than what it
replaced. On an exponential settle `dR/dt = -R/tau`, so releasing once the rate
falls below `r` cents per second leaves `R = r tau` still to come, and `tau`
belongs to the string rather than to the display: the frame rate drops out.
With the measured `tau` of 0.38 s, `r` has to be under about 2.6 to land inside
a cent, and 1.3 is taken, which now measures between **0.17 and 0.46 cents**
remaining across every frame rate from 5 to 60 per second.

What it costs is about a second and a half of waiting. That is not a defect to
be tuned out: it is how long the string takes, and it is about how long a
tuning pedal appears to take. A tighter figure does not make the tuner faster,
it makes it read sharp.

### What was deliberately not sourced, and is marked as such in the header

- **No published gross error figure for YIN on guitar or bass.** The paper's own
  words: it "has been only informally evaluated on music". The error rates
  above are speech corpora and are not quoted as instrument figures anywhere in
  the code or the test.
- **The cause of step 5's interpolation bias.** The paper states the bias and
  the remedy and not the reason.
- **No measurement of how far a plucked string's second harmonic exceeds its
  fundamental.** The CCRMA, UNSW and Penn State sources were read and none gives
  a formula for plucked harmonic amplitudes against pluck position. So no
  numeric claim about harmonic amplitudes is made. The general argument for a
  time domain method stands on the missing fundamental and middle harmonic
  sources quoted above, which are sourced.
- **The McLeod Pitch Method could not be obtained.** The ICMC 2005 paper is
  linked from the authors' own page but the href is stripped in conversion, a
  constructed URL was refused, and it is indexed nowhere else reachable. The
  author's thesis gives the SNAC function, but its chapter 6, which holds the
  peak picking rule, is past the text extraction cap. The threshold 0.93 that
  circulates for it is available only from a third party's API documentation
  reporting what the paper uses, so it is not written down anywhere here. This
  is the reason YIN was chosen over MPM and not a judgement about which is
  better: YIN could be had complete, and this project cannot cite a constant it
  only has second hand.
- **ISO 16 and the MIDI specifications** are paywalled or behind a login. The
  equal temperament formula is cited to Wolfe's page at the University of New
  South Wales rather than implying a MIDI document that was not read.

### What finishing it on 10-29 found

The JUCE half is small: a tap in the device callback, a thread that runs the
estimator, and a readout. The work was in what the mutation run and the audit
said about the half that had already landed.

**Seven mutations of `Tuner.h` survived the test as it stood**, and not one of
them was a fault in the estimator. Every one was a check that could not see the
thing it was written for, and two of them are worth remembering as shapes
rather than as incidents:

- **A check whose own bound is computed from the constant it is checking is not
  a check.** The glide test bounded how far a reading could be from the settled
  pitch by `stabilityCents * stabilityFrames / (1 - exp(-dt/tau))`, read
  straight out of the constant under test. Loosen the constant twelve times and
  the bound loosens twelve times with it, so a gate that opened at 3.95 cents
  out, four times the tier this tuner claims, passed. The same shape killed the
  level floor check: the quiet test signal was computed from the floor, so a
  floor of -200 dB, which is a tuner chasing a closed input's own noise floor,
  passed too. Both now use absolute figures and a bound derived from the
  window's permitted spread rather than from the constant.
- **A check can test the wrong half of the thing it names.** The check written
  for "the stability history does not carry across notes" played 220 Hz and
  then 330 Hz, and a fifth disagrees with every stale frame, so the stability
  test holds the display back whether or not the history was cleared. Removing
  the clear entirely passed it. The same note either side of the silence is
  what tests it, and the fault it guards against is narrow and real: a plucked
  string is flattened by roughly the twelve cents of its own attack glide, so
  its first sharp reading can land within a third of a cent of the previous
  note's settled pitch, and the tuner then says "in tune" on a string that is
  twelve cents flat.

The other five: a silent buffer reporting perfect clarity, which is a display
reading "rock solid" on an unplugged socket; step 5's interpolated depth, the
half the header singles out as the detail that decides whether the paper was
implemented or read, invisible at a threshold of 0.1 and caught by tightening
the threshold instead; Eq. (8)'s cumulative mean taken over one term too many,
which is a bias towards reading high at short lags and changes no reading on
anything the suite generates, caught against the **exact** closed form of a
sinusoid's difference function (the asymptotic form agrees only to five per
cent, so the measurement's own floor would have been larger than the fault);
and the ring's tear check, where removing it, loosening it fourfold and
replacing it with a did-anything-arrive test all passed, because single
threaded the two counter reads inside `readLatest` have nothing between them.
That one now has a writer behind it, which is why `Tests/run.sh` gains
`-pthread`.

**Three breakages are deliberately not caught**, and are named in the test so
nobody hunts for the check that should have caught them: the tear bound
loosened by a factor of four rather than removed, which would need a reader
starved for between two and eight seconds inside a copy of a few tens of
microseconds, and the two memory orders, which only a ThreadSanitizer build
could see and `run.sh` builds with AddressSanitizer.

**The audit found two faults in the new code and a pile of overclaimed
comments.**

- **The display held a dead note for ever.** The ring's write counter is
  monotonic and nothing resets it, so "enough audio has arrived" latches true
  the first time it does, and the ring goes on holding the last tenth of a
  second of whatever was played. An estimator handed the same buffer every pass
  reads it as perfectly periodic. So an interface unplugged mid session, or a
  machine that slept, left a bright settled note on screen for the rest of the
  session with nothing to say it was not live. Whether audio is still arriving
  is a question about the counter **moving**, not about its value.
- **The estimator's cost grows as the square of the sample rate**, and five
  places stated nine milliseconds a reading as though it were a property of the
  code. Measured: 9 ms at 44.1 kHz, 11 at 48, 48 at 96 and 197 at 192. At 192
  kHz, asking for a reading every hundred milliseconds spins a core for as long
  as the window is open, in a process that is also running a device callback.
  The thread now sleeps for three times whatever the reading cost, which holds
  it under a quarter of a core at every rate the studio runs at, and a slower
  reading rate makes the gate stricter rather than wrong.
- **The window was modal**, copied from `showPreferencesWindow`, where modality
  is right. A tuner is used while a session is live, and a modal one takes the
  transport, the mixer and every input gain with it.

Plus: the test covered 44.1 and 48 kHz only, where every other arithmetic test
here covers 96 and 192, so the longest buffer the estimator ever handles had
never been exercised; and several comments said more than the code did, which
is the failure this project keeps meeting. The gate's own figure was derived
for one step rather than for four frames agreeing, understating its own 1.40
second wait as nine tenths of a second; the ring's slack was quoted from a
search floor that was later lowered five semitones; the operation count was out
by a factor of seven; `norm[0] = 1.0` is a dead store whose comment credited it
with a guard that belongs to the normalisation as a whole; and the reference
range was called the widest documented when this very file tabulates a wider
one.

### What a second audit found, after the wiring had landed

A separate auditor and a separate mutation harness went over the arithmetic
while the wiring was being written, between them 88 deliberate breakages, each
compiled with the sanitizers `Tests/run.sh` uses. Six of the breakages are
caught by AddressSanitizer alone and produce no failing check at all, so a
plain `-O2` harness would have called them survivors, which is the thing
`CLAUDE.md` warns about and is now twice demonstrated on this project.

Four findings, worst first.

**The gate's accuracy depended on how often the caller asked it for a
reading**, and this is the one worth remembering. It asked that the last four
readings lie within a quarter of a cent of their mean: the obvious thing to
write, and it has no notion of time, so the spread it measures is a function
of the frame rate. Measured on the glide, varying nothing else:

| readings per second | gate opens at | sharp when it opens |
|---|---|---|
| 10 | 1.40 s | +0.37 cents |
| 15 | 1.23 s | +0.57 cents |
| 20 | 1.05 s | +0.93 cents |
| 30 | 0.86 s | +1.50 cents |
| 60 | 0.56 s | +3.32 cents |

As wired it was not broken. The readout analyses on its own thread paced at a
hundred milliseconds and leaves the thirty hertz timer to repaint, and that
pacing was chosen deliberately and commented. But it held together only
because a constant in the window matched an assumption in `Tuner.h` with
nothing checking that it did, which is exactly the arrangement this file
already records going wrong: the record of the recording alignment says "the
fourth thing wrong with it is why: the figure was in the window, where nothing
tests it". A tuner reading one and a half cents sharp on every plucked note
was one timer interval away, and the test could not have seen it because it
fixed the hop.

The criterion is a rate now, and the arithmetic is simpler than what it
replaced. On an exponential settle `dR/dt = -R/tau`, so releasing once the
rate falls below `r` cents per second leaves `R = r tau` to come, and `tau`
belongs to the string rather than to the display: the frame rate drops out.
The readout passes its own pacing figure as `GateSettings::frameSeconds`
rather than agreeing with anything. Measured, the first reading shown now sits
between 0.17 and 0.46 cents from where the string settles across every rate
from 5 to 60 a second, and the test sweeps the rate rather than fixing it.

**Above the search ceiling it reported an exact octave down, confidently.** A
note above the ceiling has no period inside the lags computed, so the first dip
that is inside the range is the one at twice the period. With the ceiling at B6
a 2.5 kHz tone read 1250 Hz and a 3.5 kHz tone read 1750 Hz, both steady, with
the threshold satisfied and nothing to say they were wrong. That is a pinch
harmonic, a whistle or a squeal, and a musician would believe it.

The ceiling is C8 now, the top of the detection range Boss print for the TU-3
and Korg for the CA-50, and the top note of a piano. It does not make the
failure impossible, it moves it above anything an instrument plays. The range
the one cent tier is claimed over stays at B6 and is a separate number, because
the low pass is designed from it: design the filter at the ceiling and almost
no filtering is left, which is measurably the difference between inside the
tier and outside it.

Lowering the shortest lag to reach C8 exposed one new limit, measured rather
than left to be found: a note with no fundamental at all reads an octave **up**
in the top semitone of the claimed range, because the lag at half its period
has come inside the searched range. Over sixty random amplitude and phase draws
per case it is clean at 200, 400, 700, 1000, 1300, 1600 and 1800 Hz, once in
sixty at 1900, and five to ten in sixty at B6. It is the first dip rule seen
from the other side, so it is structural rather than a bug, and it is a signal
no instrument makes: a note at 1900 Hz with no energy at 1900 Hz.

**B6 was never tested, by any sweep in the file**, because the constant naming
it was `1975.533205` and B6 is `1975.5332050245`. A literal rounded down is
below the note it names, so every sweep that stopped there dropped the one note
the header calls the top of the range, and a breakage that made the highest
note read an octave down went unnoticed through a whole run. Worth filing next
to the other faults here that were a digit rather than a decision.

**No test signal decayed**, which is what a plucked string does and what this
tuner exists to read. There is one now, and it answered the open question the
mutation run had raised about the clamp on the interpolated shift. A decaying
signal makes the difference function rise with lag, because the two windows
being compared no longer hold the same energy, so the dip is pulled off the
true period and the clamp can be the less accurate of the two options. But
where that costs more than a cent, the note is dying fast enough that the
buffer is not periodic any more and the threshold has already thrown the
reading away: the error never reaches the display. Every note with a T60 from
ten seconds down to seven tenths reads inside a third of a cent; below that the
readings run to several cents and every one of them is rejected.

Also found and fixed: the glide's derived bound was loose by a factor of eleven
and would have accepted a gate showing the string 5.16 cents out; three
comments said the opposite of the code, the filter consuming its extra samples
at the newest end rather than the oldest, the stability history being in
absolute cents rather than note space, and the value at lag zero being written
but never read; and the required ring capacity is a correctness requirement
rather than a performance tip, `capacity >= snapshot + 2 * largest block`,
written as one now with the measured tearing rates behind it.

### What is left, and is the next night's work

**Decimate the tuner's tap to a fixed analysis rate.** This is the proper fix
for the cost above, and it is a research job rather than a design one, which is
why it was not done tonight. The highest note the one cent tier is claimed to
is B6 at 1975 Hz and the search ceiling is C8 at 4186, so the analysis needs
nothing like 192 kHz, and running at a fixed rate would make the cost constant
at every device setting. What it needs first is the decimation filter's
specification: the square window already in front of the estimator is designed
for 1 dB of droop at the top of the claimed range and has 13 dB sidelobes,
which is nowhere near enough stopband to decimate through, so whatever folds
would land in the band the estimator is reading. A night should go to the
filter, with a stated stopband and a measurement, rather than to dropping
samples and hoping.

**That paragraph used to end by saying the frame spacing was no longer part of
this, on the grounds that the gate's criterion is a rate and the readout hands
it its own pacing figure. Both halves of that were wrong**, and the entry at the
top of this file dated 2026-11-06 "later the same day" says how. The readout
handed it a constant rather than its own figure, and the spacing turned out to
decide something the rate criterion cannot see at all, which is a second and
larger reason to want the decimation described here.

Two smaller things, recorded rather than done:

- **`AudioEngine::sampleRate` is a plain `double`** written by the device
  thread and now read by the analysis thread as well as the message thread.
  Formally a data race, harmless in practice on an aligned double, and not
  introduced here: the message thread already read it that way. Worth making an
  atomic on a night that is touching the engine's state anyway.
- **The ring's samples are plain `float` loads and stores** across two threads
  by design, which is the usual tear and detect pattern and is benign on every
  target this builds for, but is undefined behaviour by the standard and would
  be flagged by ThreadSanitizer. `std::atomic_ref<float>` with relaxed ordering
  is the formal spelling.

### Rejected

- **Plain autocorrelation.** Rejected on the research rather than on taste, and
  rejected precisely where a bass needs it: McGonegal, Rabiner and Rosenberg,
  IEEE Trans. ASSP 25(3), 1977, found "the AUTOC method performed worst on
  low-pitched speakers and best on high-pitched speakers", and that it needs
  contour smoothing to be usable at all.
- **Porting an existing implementation.** aubio is GPL and TarsosDSP is GPL
  family, and while GPL-3.0 and AGPL-3.0 may be combined through section 13 of
  each, neither is the paper. The equations above are the whole algorithm and
  implementing a published algorithm from its description depends on nobody's
  code. librosa's `yin` is ISC licensed and cites the same paper, and the right
  use of it is offline, to generate expected values to check against, which is
  what was done rather than porting it.
- **A fixed 1 kHz low pass, as the paper used.** The paper low passes by
  "convolution with a 1-kHz square window" and that is kept, but not at 1 kHz.
  A square window of length `sr/1000` has its first null at 1 kHz, and measured
  against its own response that puts a note at the top of a tuner's range 28 dB
  down, which is throwing away the signal the tuner is there to read. The length
  is chosen from the highest note being searched instead, by a stated droop
  criterion, and the header says that this is a departure from the paper and
  why. The paper's own warning in the other direction is respected: the cutoff
  is never set from the detected pitch, which would be a feedback path.

---
## 2026-10-27: the fader law, and where unity sits on the travel

### Status: built, tested, audited, and waiting on a human to merge it

Pull request **#59**, marked ready for review. The tests job, `ubuntu-latest`
and `windows-latest` are all green; `macos-latest` was still building, which is
routine here and has not failed. The release notes are on the branch rather
than written after the merge, so they go in with it.

The session that built it was **not permitted to merge without review**, so the
merge is the owner's to make. Nothing is left to do on the branch. A later
night should not pick the fader law up again: if #59 is still open, it needs a
click and not more work, and if it has been merged then this entry is the
record of it.

### What was looked at

| Candidate | Verdict |
|---|---|
| **The fader law** | **Chosen.** Deferred on 2026-10-08, 10-13, 10-15, 10-21 and 10-22, each time as a real gap that wanted a night of its own because it changes the meaning of every stored `volume` in every saved project. This is that night, and the night of 10-22 had already narrowed what is actually wrong, which is what makes it buildable in one sitting. |
| MIDI record alignment | Deferred a sixth time, same reason and no new one: the figure is easy and knowing whether the keyboard is being played live or a pattern is being bounced off the grid is not. |
| Punch in and punch out recording | Deferred, same reason as 10-21 and 10-22. Still wants a published answer to what happens to the audio already on the track, and still reaches `Project`, `ProjectIO`, `CompModel`, the playlist and the engine at once. |
| A tuner | Deferred. Still wants a published pitch detector and a window. |
| Noise shaping on export | Deferred, not re-rejected. The 10-22 entry left it as "worth a night of its own, from the paper itself, with the filter designed at the running rate", and nothing about tonight changes that. |

### A correction to the 2026-10-22 entry

That entry says the fader's ceiling is **+3.88 dB**. It is **+7.75 dB**. The
law is `(v / 0.8)^2`, so the ceiling is `(1.25 / 0.8)^2 = 2.4414` as an
amplitude, and the entry converted `1.25 / 0.8 = 1.5625` to decibels without
squaring it first. The 2026-10-13 entry had it right at "about +7.75 dB".
Recorded rather than quietly fixed, because the wrong figure is half as far out
as the right one and either would have read as plausible.

Everything else in that entry survives measurement, including the part that
matters most: **the taper is not the problem.** `(v / 0.8)^2` gives the -20 dB
to -6 dB region 25.07 per cent of the travel, against 28.17 for Ardour and
22.50 for the X32. Three nights in a row wrote down "the taper is wrong" as the
reason to do this work, and it was not the reason.

### What was chosen, and why

What is genuinely wrong is three things, none of them the curve's shape:

- **Unity sits at 0.640 of the travel**, where every reference puts it between
  0.75 and 0.782. A fader whose 0 dB point is almost two thirds of the way up
  has a third of its travel spent on boost nobody uses.
- **The ceiling is +7.75 dB**, which is `1.25` divided by `0.8` and then
  squared. Nobody chose that number, and it is the same category of accident
  as a magic constant.
- **There is no defined floor.** The bottom of the travel is gain 0 by
  arithmetic rather than by decision, and no part of the scale is written down,
  so the number next to the fader cannot be checked against anything.

The law adopted is the **Behringer X32's**: piecewise linear in decibels in
four segments, unity at exactly 0.75 of the travel, ceiling +10 dB, and exact
silence at the bottom. Three reasons, in order of weight:

1. **Its inverse is exact and cheap.** A fader position has to round trip
   through a saved project file and through automation curves without drifting,
   and the old stored positions have to be translated by converting the old
   gain to the new position. Within a segment the law is affine in decibels, so
   it inverts to a floating point unit by construction: measured max position
   round trip error 1.11e-16 against Ardour's 1.33e-15. Ardour's law needs
   `pow(x, 8)` against `pow(x, 1/8)`, a matched pair of transcendentals.
2. **Unity lands on a number that can be printed and typed.** Exactly 0.75,
   and every labelled tick lands on a dyadic fraction of the travel: 1/16, 1/8,
   3/16, 1/4, 3/8, 1/2, 5/8, 3/4, 7/8, 1. Ardour's unity is `(32/33)^8 =
   0.7817867535191608`, which cannot be drawn as a tick without writing that
   expression into the layout code, and which **moves to 0.6616 the moment the
   ceiling is raised past +6 dB**, because Ardour rescales the gain rather than
   the position. The one virtue Ardour's law has does not survive asking it for
   a different ceiling.
3. **The owner records through a Behringer UMC1820.** Behringer's own fader
   convention is the one he is most likely to already have in his hands, and it
   is the one reference here whose scale can be held against a photograph of
   real hardware rather than against another program.

The ceiling being a number somebody chose is the point of it. Ardour's
+6.0206 dB is the decibel value of the coefficient 2.0, which is the same kind
of artefact as the +7.75 dB being replaced; Behringer printed **+10** on the
strip.

### Where the behaviour came from

**The law itself could not be obtained from Behringer.** Said plainly, because
this project keeps a record of the shape of failure where a number is present,
plausible and unsourced. What was obtained:

- **Behringer's own printed fader scale**, from the X32-TP operation manual's
  channel strip artwork: the ticks are **+10, +5, 0, -5, -10, -20, -30, -40,
  -50, -60, -inf**, on a 100 mm motorised fader. That is the manufacturer
  stating the ceiling, the floor and the intermediate scale.
- **Two independent open source implementations that agree to the digit**:
  `jtsage/x32_osc_state` (`src/enums/mod.rs`, `level_to_string` and
  `level_from_string`) and `bitfocus/companion-module-behringer-x32`
  (`src/util.ts`, `floatToDB` and `dbToFloat`). Both write the same four
  affine segments with the same breakpoints.
- **A fourth corroboration** from Behringer's X32 MIDI table: "Faders - Value
  95 = 0db" for 7 bit fader controllers, and `0.75 * 127 = 95.25`, which
  rounds to 95.

The strongest part of that evidence is the printed scale itself: within each
segment Behringer's ticks are **equally spaced in decibels**, 5 dB apart in the
top segment and 10 dB apart in the two below it. That is what a piecewise
linear in decibels law predicts and is not what any smooth law gives, so the
four segments are derived from the manufacturer's own artwork rather than taken
on the implementations' word. The segments were derived from the eleven row
tick table rather than transcribed from either implementation's code.

| Segment | Position | Decibels | Forward | Inverse |
|---|---|---|---|---|
| 4 | 0.5 .. 1.0 | -10 .. +10 | `dB = 40p - 30` | `p = (dB + 30) / 40` |
| 3 | 0.25 .. 0.5 | -30 .. -10 | `dB = 80p - 50` | `p = (dB + 50) / 80` |
| 2 | 0.0625 .. 0.25 | -60 .. -30 | `dB = 160p - 70` | `p = (dB + 70) / 160` |
| 1 | 0 .. 0.0625 | -90 .. -60 | `dB = 480p - 90` | `p = (dB + 90) / 480` |
| - | p = 0 | -inf | gain = 0 exactly | - |

The eleven row tick table, which is what the test asserts:

| dB | Position | As a fraction | Gain |
|---|---|---|---|
| +10 | 1.0000 | 1 | 3.16227766017 |
| +5 | 0.8750 | 7/8 | 1.77827941004 |
| **0** | **0.7500** | **3/4** | **1.0** |
| -5 | 0.6250 | 5/8 | 0.562341325190 |
| -10 | 0.5000 | 1/2 | 0.316227766017 |
| -20 | 0.3750 | 3/8 | 0.1 |
| -30 | 0.2500 | 1/4 | 0.0316227766017 |
| -40 | 0.1875 | 3/16 | 0.01 |
| -50 | 0.1250 | 1/8 | 0.00316227766017 |
| -60 | 0.0625 | 1/16 | 0.001 |
| -inf | 0.0000 | 0 | 0.0 exactly |

**Ardour's law, read from its own source** and rejected rather than guessed at.
It is `gain_to_position` and `position_to_gain` in `libs/pbd/pbd/control_math.h`
lines 28 to 46, not in `utils.cc` where it is usually said to be:
`p = ((6 log2(g) + 192) / 198)^8`, with unity at `(32/33)^8` and a ceiling of
exactly gain 2.0, which is +6.0206 dB. Its ceiling is the config file only
variable `max-gain`, not exposed in Ardour's preferences, and
`gain_to_slider_position_with_max` rescales the **gain**, which is why its
unity point moves when the ceiling does.

**On the authority of any of this**, from Ardour's own author, Paul Davis, on
linux-audio-dev in May 2009: Ardour's mapping is "rather arbitrary but
definitely comfortable", and "the specs for various standard meters and for
various standard faders do not use the same scale". Nothing standardises a
fader law, and the backlog should stop implying one exists.

### What was confirmed as a trap rather than a source

**`iec_scale` is not a fader law and must never be used as one.** It is
`log_meter` in Ardour's `libs/ardour/ardour/logmeter.h`, sitting literally next
to `meter_deflect_vu`, `_ppm`, `_din`, `_nordic` and `_k`. It is a **meter
deflection** function: unity at `100/115 = 0.8696` of the scale, and a hard
floor that returns 0 below -70 dB. It is the single most widely copied function
in amateur audio code and copying it here would implement a VU meter's needle
as a fader's travel. Checked: it appears nowhere in this repository.

**IEC 60268-17 is the standard volume indicator and IEC TR 60268-18 is the
digital peak programme meter.** Neither is a fader law, and the AES standards
list has nothing on faders either. Anybody who reaches for a standard number
here will find a meter and implement the wrong thing.

### The ceilings other studios ship, and what could not be sourced

| Studio | Ceiling | Source |
|---|---|---|
| Cubase and Nuendo | **+12 dB**, and +6 dB for projects made before Cubase 5.5 | **Primary.** Steinberg's own Project Setup documentation, "Volume Max" |
| Ardour | **+6.0206 dB** (`max-gain = 2.0`), config file only | **Primary.** Its own source, and its author in writing |
| Behringer X32 | **+10 dB** | **Primary.** Behringer's own printed fader scale |
| Harrison Mixbus | Presumed Ardour's, being Ardour derived | **Inferred, not sourced.** Harrison's manual states no range |
| Reaper, Pro Tools, Logic, Studio One | **Could not be obtained** | Nothing. Reaper's user guide PDF truncates before the page, and no Avid, Apple or PreSonus hosted document reachable from here states a fader range |

The widely repeated figures for those last four, +12 for Pro Tools and Reaper,
+6 for Logic, +10 for Studio One, are **recorded here as unsourced** so that no
later night supplies them from memory. There is no consensus ceiling: +6, +10
and +12 are all shipping in programs whose documentation says so.

Logic does document the gesture rather than the range, which is worth having:
"Option-click the Volume fader to return it to a neutral level (0 dB gain)".
Ardour does the same through `gain_slider->set_default_value`, and **neither
has a detent or a snap on the fader**. Checked in Ardour's tree: the only
detents are on the panner and two monitor section knobs. A snap at unity would
make -0.3 dB unreachable, so there is none here either, and a double click
returns to exactly unity.

### The decisions taken without a primary source, recorded as decisions

**The jump to silence at the very bottom.** The continuous part of the law
reaches -90 dB at the bottom of the travel and position 0 is exact silence, so
there is a discontinuity of -90 dB at one point. That is what both references
do: Ardour special cases `pos == 0.0` to return 0.0 and its continuous part
only reaches -192.66 dB, and Behringer prints -inf below a scale that stops at
-60. The argument for it is that the step is **about one least significant bit of a
16 bit file**: -90 dB is an amplitude of 3.1623e-5 and 1/32768 is 3.0518e-5,
which is -90.31 dB, so it is 1.036 of one bit.

An earlier version of this paragraph, and of the header's, said the step was
**below** a 16 bit bit, which is false by 0.31 dB in the direction that
flattered it. Found by the test refusing to assert it, and worth leaving the
record of rather than quietly correcting, because it is the one number this
decision rests on and the plausible wrong version of it read perfectly well.
One bit at the bottom of a 16 bit file is still below what any converter
resolves, so the argument survives being stated correctly.

The alternative, making the bottom segment approach -inf continuously, was
rejected: the gain would never reach zero, and a mixer summing sixteen inserts
at -70 dB each is not silent. A fader pulled all the way down has to give
nothing.

**The slope is discontinuous at the three breakpoints**, jumping from 40 to 80
to 160 to 480 decibels per full travel. This is the real console's behaviour and
is why Behringer's printed ticks change spacing at -10 and -30, so it is a
modelled behaviour rather than a defect, but it means an automation ramp
crossing a breakpoint has a kink in its fade rate. Written into the header so
the next person does not read the kink as a bug and smooth it out.

**Automation curves keep storing the fader position**, so a line drawn straight
in the lane is straight in decibels within a segment, which is what a fade is
supposed to be. The alternative, storing gain, would make a drawn straight line
a fade that is fast at the start and slow at the end.

**No 1024 step quantisation.** The X32 quantises its fader to 1024 steps
because its fader is a physical component behind an analogue to digital
converter. These positions are floating point numbers. The figure is recorded
in case a hardware control surface ever arrives: the finest step would be
40/1023 = 0.0391 dB at the top and 480/1023 = 0.469 dB at the bottom.

### The migration, which is the dangerous half

Every existing project stores a fader position in 0 .. 1.25 under the old law,
and every existing automation curve stores a value in 0 .. 1 that was multiplied
by 1.25 to reach that same travel. Both have to be translated, and **the
translation goes through gain, not through the fraction of the travel**.

Translating the travel fraction instead is the trap, and it is worth writing
down because it looks right: old unity is at 0.640 of the travel and new unity
is at 0.75, so a fader stored at 0.8 would come back at a position of 0.64,
which is **-4 dB**. Every track of every existing project would come back
quieter, by an amount that depends where its fader was, and it would present as
"my old mixes all sound wrong and the balance has moved" with nothing anywhere
in the audio path to blame for it.

Every old gain fits inside the new range, because the old ceiling of +7.75 dB
is below the new +10 dB, so no existing project loses anything at the top.

### What the migration cannot do, found by the audit

Translating a curve translates its points and **not the line between two of
them**, because the two laws interpolate differently: a straight legacy segment
was linear in position under `(v / 0.8)^2`, and a straight segment now is
linear in decibels. So a migrated fade keeps both ends exactly and changes
shape in between, by an amount that depends on how far it travels:

| The fade | Worst departure from its old shape |
|---|---|
| 0 dB to -3 dB | 0.065 dB |
| 0 dB to -10 dB | 0.716 dB |
| 0 dB to -20 dB | 0.570 dB |
| 0 dB to -60 dB | 13.600 dB, worst at 87 per cent through |
| 0 dB to silence | 7.959 dB at the midpoint, 15.918 at three quarters |

So a short fade is untouched and a long fade out drops away noticeably quicker
than it did. `point.bend` is carried across untranslated as well, so a bent
legacy segment is reshaped twice.

Worth being clear that the new shape is the better one: a fade linear in
decibels is what a fade is supposed to be, which is the same argument
`AutomationCurve.h` already makes about its own bend. That is a reason the
change is acceptable and it is not a reason to leave it unsaid, which is why it
is in the header, here, and in the release notes.

**Subdivision, deferred to a night of its own.** Holding the old path means
sampling the legacy curve and emitting straight pieces in the new space, which
turns somebody's two point fade into a nine or seventeen point one: the gesture
they drew stops being editable as the gesture they drew. It also needs its own
care at the tail of a fade to silence, where the old path runs to minus
infinity and the new one stops at -90 dB, so a naive subdivision measures worse
than no subdivision at all there. That is a feature with a test, not a line
inside a loader. Whoever takes it should decide first whether preserving the
drawn path is worth the point count, because it may well not be.

`FaderLawTest.cpp` asserts the table above, so the gap cannot quietly become
something else.

### Where the arithmetic lives

`Source/FaderLaw.h`, with no JUCE in it, so the whole law and the whole
migration are testable under sanitizers. The engine keeps only the load and the
one multiply. This is the same split `PanLaw.h` already uses and for the same
reason: a fader law a decibel out does not error, does not crackle, and looks
entirely normal on a meter.

---

## 2026-10-22: a bit depth to export at, and the dither that makes it honest

### What was looked at

| Candidate | Verdict |
|---|---|
| **A bit depth choice on export, and dither on the fixed point conversion** | **Chosen.** Export was hard coded to 24 bit (`ExportJob.h`, `.withBitsPerSample (24)`) with no choice anywhere, and the word "dither" appeared nowhere in the repository. Rejected on 2026-10-08 with an explicit precondition: "there is nothing for it to fix until a 16 bit export option exists". This is the night that builds the option and the dither together, because either alone is half a feature. |
| Fader law | Deferred a fourth time, and the research changed what the next night should actually fix. The premise written down on the last three nights, that the taper is wrong, does not survive measurement: `(v / 0.8)^2` over a travel of 0..1.25 gives the -20 dB to -6 dB region 25.1 per cent of the fader, against 28.2 for Ardour's law and 22.5 for the X32's, and it is slightly finer than Ardour at unity (0.271 against 0.308 dB per one per cent of travel). What is genuinely wrong is narrower and should be written down as such: **unity sits at 0.640 of the travel** where every reference puts it at 0.75 (Behringer X32, exactly) to 0.782 (Ardour, `(32/33)^8`), and **the ceiling is +3.88 dB**, which is an artefact of 1.25 divided by 0.8 rather than a number anybody chose. Nothing in the audio world specifies a fader law: IEC 60268-17 is the VU meter and 60268-18 is the digital peak meter, and the widely copied `iec_scale` piecewise function is a **meter deflection** function that lives in Ardour's `logmeter.h` next to `meter_deflect_vu`, puts unity at 0.87 of travel and has a hard floor at -70 dB. Anybody reaching for it as a fader law would be implementing the wrong thing, which is why it is recorded here. |
| Punch in and punch out recording | Deferred, same reason as 2026-10-21 and no new one. |
| MIDI record alignment | Deferred again, same reason as the last four nights. |
| A tuner | Deferred. Still wants a published pitch detector and a window. |
| Noise shaping | **Rejected, and the reason is this project's own history rather than taste.** The citation is real: Lipshitz, Vanderkooy and Wannamaker, "Minimally Audible Noise Shaping", JAES vol 39, pp 836 to 852 (1991), whose abstract claims a second order shaper is worth 10.9 dB of perceived noise against Fielder's modified E-weighting, within 0.6 dB of the information theory limit. The five tap set in circulation, `{ 2.033, -2.165, 1.959, -1.590, 0.6149 }`, could **not** be verified against the paper: it reaches us through SoX into FFmpeg's `noise_shaping_data.c` and separately through Steve Harris into Ardour's gdither, both secondary, and **the two disagree about which sample rate it is for**, gdither's own comment saying "only really works for 46kHz-ish signals" against FFmpeg filing it under 44100. Shipping five coefficients of unverifiable provenance, specified at one rate, in a studio that runs at five, is exactly the failure `Fx/Tilt.h` exists to avoid. It is also the wrong trade to make first: shaping buys perceived quiet by making the file measurably noisier. Worth a night of its own, from the paper itself, with the filter designed at the running rate. |
| High pass (first order shaped) TPDF | **Rejected for tonight, with its figures recorded because they are worth having.** Differencing successive uniform draws rather than summing two fresh ones leaves the marginal density triangular, so Theorem 8 still holds and the moments are still flat, but the error spectrum follows `q^2/12 + 4 sin^2(pi f / fs) q^2/12`: measured 4.8 dB quieter below 1 kHz and 2.2 dB louder near Nyquist, for **identical total power**. That is a real win at no cost in level, and it is what Ardour ships under the name "Triangular". It was not chosen because the primary source, Lipshitz and Vanderkooy, "High-Pass Dither", AES 4th Regional Convention, Tokyo (1989 June), pp 72 to 75, **could not be obtained**, so the construction here would rest on derivation and measurement rather than on the paper. Flat TPDF is the one the definitive survey recommends by name. |
| Rectangular dither | **Rejected on the research.** It is in every implementation's menu and it is a worse answer, not a cheaper one. The 1992 survey, section 5.3: 1 LSB rectangular dither makes the first error moment zero for every input, and then "The error variance, on the other hand, is clearly signal dependent. This is sometimes referred to as noise modulation and is audibly undesirable." Measured over a DC sweep across one step, its error variance swings from **0 to q^2/4 and back, following the signal**: the whole noise floor rises and falls with the waveform. Offering it would be offering a mode whose only distinguishing property is a fault. |
| Dither on the recorder | Rejected as a non-problem, which is worth saying so nobody adds it. A take is captured from the interface and written at the device's own word length, so there is no word length reduction on that path and nothing to linearise. Dither belongs where a float mix becomes a fixed point file, which is the export and only the export. |

### What was chosen, and why

**There was no way to produce a 16 bit file, and no dither to produce one honestly with.**

Every export was 24 bit whether it was a working master or something to be sent
somewhere that wanted 16 bit, so getting a 16 bit file meant converting it in
another program, which is both an extra step and a conversion this studio then
knows nothing about.

The dither is the half that is not obvious, and it is the reason this is a
night's work rather than a combo box. Reducing a 32 bit float mix to 16 bit
quantises it, and an undithered quantiser's error is a deterministic function
of the signal rather than noise: it is correlated with the waveform and it
arrives as distortion rather than hiss. Measured at 16 bit and swept across
amplitude, the worst harmonic of a low level sine stands at least **36.6 dB**
clear of the record's own noise floor undithered and at most **4.1 dB** clear
of it dithered, which is to say nothing stands out of the noise at all.
It is also where the noise floor goes when the music gets quiet: the error
variance of an undithered 16 bit quantiser wanders over 3.5 dB as a sine fades,
so reverb tails and fade outs get grainy in a way that follows the music. That
is the shape of fault this file keeps meeting. The file plays, the meters look
right, and the tails are wrong.

Both halves had to land together. A 16 bit option without dither would be a
new way to produce a worse file, and dither without a 16 bit option fixes
nothing, which is precisely why it was rejected in October.

### Where the behaviour came from

Primary sources, with what each is actually load bearing for.

**The dither itself.** S. P. Lipshitz, R. A. Wannamaker and J. Vanderkooy,
"Quantization and Dither: A Theoretical Survey", JAES vol 40, no 5, pp 355 to
375 (1992 May), read as full text. Its section 5 covers nonsubtractive dither,
which is what this is: the dither is added before the quantiser and nothing is
subtracted afterwards. What was taken from it:

- **Theorem 8.** Dither formed by summing `n` independent zero mean rectangular
  processes each 1 LSB peak to peak "renders the first n moments of the total
  error independent of the system input", with "a total error power of
  (n + 1)D^2/12" for n at least 2. So two summed draws, which is triangular on
  plus or minus 1 LSB, flattens both the mean and the variance, and the total
  error power is `q^2/4`.
- **Why two and not one, and not three.** After equation (108) the survey calls
  2 LSB peak to peak triangular dither "unique and optimal in the sense that it
  renders the first and second moments of the total error input independent,
  while minimizing the second moment", and section 5.4 item (5) adds that it
  "incurs the least increase in the rms total error level". Note the proof of
  that uniqueness is **not** in this paper: it is deferred to reference [3],
  which was then unpublished and is now Wannamaker, Lipshitz, Vanderkooy and
  Wright, "A Theory of Non-Subtractive Dither", IEEE Trans. Signal Process.,
  vol 48, no 2, pp 499 to 516 (2000). That paper was not read (paywalled), so
  the uniqueness claim here rests on the survey asserting it.
- **The quantiser the theory requires**, which is the part that would have
  silently defeated the whole thing. Section 2: "Quantizers of the midtread
  variety will be assumed", built on the floor operator, with the quantisation
  error having "a maximum magnitude of 0.5 LSB" and a period of 1 LSB. That is
  `floor(w/q + 1/2)`, which is round to nearest, and the paper never discusses
  truncation at all.
- **The recommendation, in the authors' own words**, section 6: "We recommend
  the use of nonsubtractive, triangular-pdf dither of 2-LSB peak-to-peak
  amplitude for most audio applications requiring multibit quantization or
  requantization operations."
- **What it does not do**, which matters for how the comments are worded.
  Section 5.4 item (1): nonsubtractive dither **cannot** make the total error
  independent of the input, only chosen conditional moments of it. So the
  claim in the header is that the first and second moments are independent of
  the signal, not that the error is independent of it, and not that the error
  is noise uncorrelated with the signal. Item (4) names the price: a total
  error variance greater than `q^2/12`, which here is the 4.77 dB the survey
  itself quotes as 4.8 dB when comparing its figures 10(d) and 6(d).

Also cited for existence and for the general argument that audio should be
redithered whenever it is requantised: J. Vanderkooy and S. P. Lipshitz,
"Dither in Digital Audio", JAES vol 35, no 12, pp 966 to 975 (1987 Dec). Only
its abstract was reachable, so no number in this studio comes from it.

**Which depths get dithered.** Ardour is the clearest and it is in the source
rather than the manual: `HasSampleFormat::update_sample_format_selection` in
`libs/ardour/export_formats.cc` force selects `D_None` and marks every other
dither type incompatible for 24 bit, 32 bit, float and double, so dither is
selectable only at 8 and 16 bit, and `SampleFormatConverter<float>::init` hard
codes `GDitherNone` so a float export cannot be dithered at all. Ardour's
tracker 0007428, asking for dither on 24 bit exports, is closed won't fix with
the reasoning that 24 bit covers about 144 dB and thermal and analogue noise
sit above that floor. Steinberg says the same from the other end: the Cubase
Export Audio Mixdown page recommends dither for the 16 bit case specifically,
and its Dither Effects page puts the audible range of the problem at "16 bit
or below". Apple's Logic bounce page: "Dithering is recommended when bouncing
24-bit recordings into 16-bit files."

So: dither at 16 bit, none at 24 bit, and structurally none at 32 bit float,
where no word length reduction happens.

Avid is the one source that contradicts itself and it is recorded rather than
reconciled. Its "When to Use a Dither Plug-In" page says to use one "when
exporting to a 16-bit or 24-bit audio file", while its own DigiRack Dither page
says to use it whenever "your destination bit depth is lower than 24-bit".
Avid also applies no dither on export at all: it is a plug-in the user inserts.

**Nothing was sourced for Reaper or Studio One.** Reaper's user guide is a PDF
on hosts the sandbox cannot reach, and the current Studio One manual, now
Fender Studio Pro 8.2, does not mention dither anywhere. No claim here rests
on either.

### The hazard found while reading JUCE, which was a live fault

This is the part worth reading before touching the export path again.

**JUCE's `writeFromFloatArrays` does not round to nearest when it narrows to 16
or 24 bit. It floor truncates with an arithmetic shift.** Read from the real
9.0.3 headers rather than from memory, as this project's build section
requires, because JUCE cannot be compiled here:

- `AudioFormatWriter::writeFromFloatArrays` (`juce_AudioFormatWriter.cpp`) hands
  the floats to the file static `convertFloatsToInts`, which is round to
  nearest **to int32**, clamped, and then calls `write (const int**, n)`.
- `juce_WavAudioFormat.cpp` dispatches that to
  `WriteHelper<AudioData::Int16, AudioData::Int32, ...>` at 16 bit and
  `AudioData::Int24` at 24.
- Those take the integer path in `juce_AudioDataConverters.h`, whose
  `Int16::setAsInt32LE` is `(uint16) (newValue >> 16)` and whose
  `Int24::setAsInt32LE` is `littleEndian24BitToChars (newValue >> 8, data)`.

An arithmetic right shift on a signed integer is floor division, not rounding.
So the effective quantiser behind every export this studio has ever written is
a floor, with a **mean error of -0.4997 LSB** and an error range of (-1, 0]:
measured, `-0.5/32768` comes back as code -1 and `+0.5/32768` as code 0. At 24
bit that offset is at -144 dBFS and inaudible, which is why nothing ever said
so, but it is a half LSB of DC in every file, and at 16 bit it would have been
at -96 dBFS.

It also would have quietly defeated the dither. Floor is still a uniform
quantiser, so Theorem 8 survives it and the noise modulation would still have
been fixed, but the constant first moment becomes -0.5 LSB instead of zero, so
the dither would have bought a flat noise floor and a DC offset with it.

The fix is not to add half an LSB and hope. The quantiser is now computed in
`Dither.h`, which is tested, and the integer code is handed to
`write (const int**, n)` shifted up by 16 or 8 so that JUCE's own shift puts
it back exactly: `(code << 16) >> 16` is `code` for every code in range, with
no float arithmetic anywhere in between. The float path is used only for the
32 bit float export, where `isFloatingPoint()` makes `writeFromFloatArrays`
pass the floats straight through without converting at all.

Worth knowing for the shape of it: three different float to integer
conversions live in that one JUCE header, and they disagree.
`Int16::setAsFloatLE` **does** round to nearest, but clamps to -32767 and can
never emit -32768, so switching paths would move the negative rail by one LSB.
Which one runs is decided by the destination type's `copyFrom`, not by
anything at the call site.

### The decisions taken without a primary source, recorded as decisions

**Clipping.** The theory excludes it by assumption. The survey's footnote 1:
"For practical purposes, this simply means that the signal is never clipped by
saturation of the quantizer", and the main text assumes "infinite quantizers".
Every theorem in it is about a quantiser with infinitely many levels, and no
manufacturer documentation read addresses overload either. The decision here is
to **clamp to the extreme code**, which is what both reference implementations
do (Ardour's gdither clamps after `lrintf` to 32767 and -32768; JUCE clamps the
float to plus or minus one before scaling, so an export clamps twice over
whatever this code does). No headroom is reserved: the dither can push a sample
past full scale by at most 1 LSB, which at 16 bit is 0.00027 dB, and reserving
room for it would make a bounce quieter than what was heard, which is the class
of fault `renderOffline` exists to prevent. The honest qualifier, which is in
the header rather than implied away: a clamped sample is not a uniform
quantiser, so the moment independence does not hold at the rails. That is
unavoidable in any finite word length and is exactly why the paper assumes an
infinite one.

**Rounding at ties.** `floor(x + 0.5)` in double, which sends ties upward,
rather than `lrint`, which follows the current FPU rounding mode and so ties to
even by default. The paper's mid tread quantiser is the floor form, and a
quantiser whose behaviour depends on a floating point mode would make an export
differ between the three platforms this studio is built for. With dither
present a tie has measure zero, so this only decides the undithered case.

**The random number generator.** The paper specifies the dither's distribution
and says nothing about how to generate it. This uses a 64 bit SplitMix64 step
per draw, which is fast, has no state to warm up and passes the distribution
tests that matter here, and the test checks the generated density rather than
trusting the generator. Each channel of each file gets its own stream from its
own seed, because two channels sharing a dither stream would put correlated
noise on both and image it in the centre of the stereo field rather than
spreading it. Ardour keeps per channel state for the same reason but draws all
channels from one global generator, which is adequate and is not the same
thing.

**Where the choice lives.** In Preferences rather than in a dialog on every
export, which is not where the studios put it: the bit depth is in the render
dialog in every one of them. It is in Preferences here because the export flow
is a file chooser launched asynchronously and threading a modal choice in front
of it is a second thing to get right, and because `Preferences.h` already has
the validation discipline and the tests for a stored choice. What makes that
safe rather than merely cheaper is that the status bar names the depth and the
dither, in words, from before the file appears until after it has, so a wrong
setting is visible at the moment it matters rather than discoverable
afterwards. It has to be on the progress line and not only on the message
before it: the audit found that message is replaced by the first tick of the
60 Hz timer, so on its own it was on screen for a sixtieth of a second.

### What the test asserts

Properties, measured, against the published constants rather than against the
implementation's own formulas. `Tests/DitherTest.cpp`.

`Tests/DitherProbe.cpp` is the sweep the quoted figures come from. It exists
because of what the audit found, which is worth keeping as a warning rather
than just fixing: the draft of this entry, of `Dither.h` and of the test all
quoted three numbers for the undithered distortion, **-26 dB**, **-31 dB** and
a **-74 dB** measurement floor, and nothing in the repository produced any of
them. All three were wrong. The real worst harmonic at that amplitude is the
third at -39.5 dB and the dithered floor of that measurement is -63 dB. The
figures look like a measurement taken at a different amplitude than the one
written down, in the one feature whose own header is about numbers that are
present, plausible and wrong.

The sweep is the other half of the lesson. Undithered distortion is not
monotonic in amplitude and is nowhere near it: it depends on where the sine's
peaks fall relative to the codes and swings by up to 14.1 dB between
neighbouring half steps, -25.4 dB at three and a half, -39.5 at four, -28.6 at
four and a half. The draft called four steps "where it is worst" and it is a
local best case, which left its fixed threshold 0.55 dB from failing. Moving
the amplitude to the more representative five would have turned the suite red
with every line of `Dither.h` correct. Sweeping twelve amplitudes and asserting
clearance over the record's own noise floor has 16 dB in hand at the tightest
point and does not depend on the record length.

### Mutation testing

**40 deliberate breakages, each compiled with address and undefined behaviour
sanitizers exactly as `Tests/run.sh` has them, 38 caught.** A plain `-O2`
harness would have reported the wrong answer for at least three of them, which
is why CLAUDE.md insists on the sanitizers.

30 over `Dither.h`, 28 caught: one draw, three draws, the same draw twice,
an uncentred draw, half and double amplitude, a generator that never advances,
real high pass dither with persistent state, floor instead of round, truncation
toward zero, ties rounding down, dither added after the rounding, no clamp, a
least significant bit of reserved headroom, no guard on a non finite sample,
the scale off by a factor of two, both extreme codes off by one, two wrong
shifts in `leftJustified`, 24 bit declared as 23, four breakages of the dither
policy, two of the stored id bounds, a broken round trip, and collapsing the
two channel seeds into one.

The two survivors were each established as genuine no-ops rather than gaps:

- **Differencing two fresh uniform draws instead of summing them.** They have
  the same distribution, because the draws are independent and the density is
  symmetric, and successive results are still independent, so it is white. The
  mutation that is not a no-op is *high pass* dither, which keeps the previous
  draw and differences against it, and that one is caught: the spectrum check
  and only the spectrum check fires, reading 1.10 in the bottom band rising to
  4.96 at the top against an expected flat 3.00. That is the header's central
  claim about why the spectrum check exists, and it is true.
- **The bound in `modeFromStored`.** With exactly two modes whose default is
  the ternary's else branch, an out of range id that fell through the guard
  lands on the default anyway, so removing the guard changes no answer for any
  input. It stops being a no-op the moment a third mode is added or the default
  moves to `Mode::none`, so two static assertions in the test now fail on that
  day and point at the comment explaining why.

10 over `Preferences.h`, all 10 caught, and this file had **no coverage at all**
until the audit found it. Dropping the `- 1` from `exportDepthFor` passed the
entire suite while giving somebody who chose 16 bit for a delivery a 24 bit
file, somebody who never opened the tab a 32 bit float file where they used to
get 24, and a status line that honestly reported the wrong depth. The same
mutation on `exportDitherFor` turned the default dither off on every 16 bit
export, which is the whole feature silently doing nothing.

Worth recording for whoever meets it next: clamping with `choiceId` **before**
taking one off is not interchangeable with doing it after. It gives the same
answer for every input but the most negative int, where the subtraction is
signed overflow rather than a wrong answer, and a settings file is plain text
that can hold exactly that. The test passes the two integer extremes for that
reason, and without them both orderings pass.

### What the second audit found

The fixes were audited again rather than taken on trust, and that pass found
six more, one of which is worth recording because it is the same fault the
first pass had just removed.

**The replacement text introduced its own wrong number.** The corrected
comment said the distortion "swings more than twenty decibels between
neighbouring half steps" and then cited three figures of its own that show
14.1 dB. The probe said twenty decibels between neighbouring *tenths*, where
the real figure is 7.2. Correcting a wrong number by writing a different
wrong number is easier to do than it sounds, and the only thing that caught
it was somebody re-measuring a sentence that already looked fixed.

**A loaded trap in the harmonic measurement.** `worstHarmonic` maximised over
every bin that passed `k != bin && k / bin <= 7`, which includes the
fundamental's four neighbour bins, because `k / bin` is 0 or 1 for all of
them. The comment directly above said those bins were leakage and not
distortion. With the sine on an exact bin they hold ordinary noise and the
measured margins do not move at all, which is why it survived. Move the sine
half a bin off the grid, which is the obvious way to make the test more
realistic, and the dithered check fails at -0.04 dB while the dither is
working perfectly and the real harmonics sit 5 dB below the noise. Whoever
did that would go looking at `Dither.h`.

**Two comments claiming more generality than the measurement carries.**
Asserting clearance over the record's own noise floor is less sensitive to
the record length than a fixed threshold, not independent of it: the
distortion sits still and the floor moves, so the worst clearance is 13.3 dB
at n = 2^13 and 36.6 at 2^17. Halving n twice to speed the section up turns
it red. The same holds at the top of the amplitude sweep, where clearance
crosses the threshold between 96 and 128 steps. Both are written down now
rather than left for somebody to discover.

**The dithered check has a seed tail nothing mentioned.** Its margin is set
by the exponential tail of one noise bin's power, not by the bin count the
comment blamed. Measured over 700 seeds against all twelve amplitudes, four
of 8400 draws cleared more than 10 dB, so about one seed in 175 would redden
it. The seed is fixed so it cannot flake in CI, but changing the seed is not
the free edit it looks like, and the comment now says so.

Also: `DitherProbe.cpp` used `std::numeric_limits` without including
`<limits>`, which compiles on libstdc++ by transitive include and nothing in
CI compiles a probe at all, so it would have been found by whoever next tried
to recheck the decision on another compiler. And a divide by zero guard had
been dropped from the floor average.


### What the first audit found

Five findings, all fixed, plus the figures above. The one worth keeping in mind
is the shape of the other two: a comment claiming the opposite of what the code
does (`// four steps, where it is worst`) and a comment claiming a test that
does not exist (`a function with a test on it`). Both read as reassurance and
both were load bearing in the reader's head and nowhere else. The fourth was
the status line being overwritten within one frame, which did not break the
feature but made the stated argument for the design untrue. The audit also
verified every JUCE 9.0.3 signature the change uses against a real sparse
checkout, which is the only compile check available here.

### Noticed while merging main, and deliberately not fixed here

**A bounce is now shorter than a loop pass, and it is a length rather than a
mixing difference.** The playlist loop change that merged while this branch was
open moved the engine snapshot to `Project::songLoopEndBeats()`, which rounds
up to the end of the bar, while `MainComponent::exportAudio` still takes
`project.songEndBeats()`, which is the end of the last clip. So a song whose
last clip ends mid bar plays to the end of that bar and exports to the clip.

Not fixed on this branch, for two reasons rather than one. It is not this
change's divergence, so fixing it here would widen a pull request about word
length into one about song length. And the right answer is not obvious: every
studio exposes a choice of render bounds, the whole project against a time
selection, so it wants the treatment the pan law and the solo questions got,
which is finding out what the convention actually is before picking one.

The mild reading is that it is harmless, because `renderOffline` is given a
four second tail so nothing decaying is cut off. The reason it is still worth a
night is that two numbers that used to be one now disagree with nothing to say
why, which is how three separate faults in the old record alignment code
survived for releases.

### A second line of work on this branch, and what came of it

This night ran twice, because the first run was cut off partway through the
audit fixes and the second picked the draft up, which is what the standing
instructions say to do with an unfinished draft. Both runs then did the audit
fixes independently and arrived at most of the same ones from the same
findings, which is worth recording for two reasons.

The duplicated work was not wasted in one specific way: the second run's audit
caught a trap in the harmonic measurement that the first run's fix would have
**introduced**. Lifting the cap on the harmonic search to "every harmonic
rather than the first seven" also lifts it onto the fundamental's own
neighbouring bins, which the comment directly above calls leakage rather than
distortion. With the sine on an exact bin those bins hold ordinary noise and
nothing moves, so it survives; move the sine half a bin off the grid, which is
the obvious way to make the check more realistic, and the dithered check fails
while the dither is working perfectly. The second run's version is the one
kept.

What the first run had that the second did not, and which is now merged in: the
32 bit float path having no non finite guard and that being deliberate, the
export being bit identical across the three platforms, `emplace` not
overwriting a duplicate target, why dither being export only is not the
divergence CLAUDE.md warns about, and two figures in the note shown to the
user. The quantisation noise of a 24 bit file is -149 dBFS; -144 dBFS is the
peak error, half a least significant bit, which is the figure `Dither.h` uses
correctly elsewhere for the DC offset this change removes. And only Ardour
enforces the 16 bit only rule in code: Cubase and Logic recommend it, which is
a weaker claim and the one their documentation actually makes.


### Where the arithmetic lives

`Source/Dither.h`. No JUCE, so the quantiser, the density and the policy are
all compiled and run under sanitizers on their own. `ExportJob.h` keeps only
what needs JUCE: opening the writer at the chosen depth and handing over
integers or floats.

---

## 2026-10-21: solo, and what has to stay audible when something is soloed

### What was looked at

| Candidate | Verdict |
|---|---|
| **Solo** | **Chosen.** There was no solo anywhere in the studio: not in the engine, not in the mixer, not in the project file. `InsertControls` carried a mute and nothing else. |
| Fader law | Deferred again, third time, and still a real gap: `processInsert` turns the fader position into gain with `(v / 0.8)^2` where every studio tapers in decibels. Still wants a night with the version translation as its subject, and solo was chosen over it because solo is missing entirely rather than merely wrong. |
| Punch in and punch out recording | Deferred, and the reason is now specific rather than general. The crossfade machinery it needs landed on 2026-10-15, but the thing it actually needs is a published answer to what happens to the audio already on the track: recording currently finds an empty track rather than punching into an occupied one, so a punch take would sum with the performance it was meant to replace. Making it replace instead means either a destructive trim of the clip underneath or turning that clip into a take folder whose comp selects the punch, and the second reaches `Project`, `ProjectIO`, `CompModel`, the playlist and the engine at once. Worth a night of its own, and worth researching how Pro Tools' playlists and Reaper's "trim content behind media items" each model it before choosing. |
| MIDI record alignment | Deferred again, same reason as the last three nights: the figure is easy and knowing whether the keyboard is being played live or a pattern is being bounced off the grid is not. |
| A tuner | Deferred. Still wants a published pitch detector and a window. |
| AFL and PFL listening | **Rejected, on the research, and not on taste.** Ardour's own monitor section page defines both as routing the listened signal to a second monitoring output while leaving the master untouched: "Listen disconnects the monitor bus from the master, so it receives only listened signals... the master is unaffected". This engine has exactly one output path. Building AFL or PFL means a second bus, a second set of device output channels to assign it to, and a monitor section to control it, which is a monitor section feature rather than a solo feature. Solo in place is the default in every studio looked at: Ardour ships `solo-control-is-listen-control = false` and `use-monitor-bus = false`, and the option editor forces the first false while the second is. |
| Solo lock, Ardour's `SoloSafeControl` | Rejected. It freezes an insert's solo state against clicks and against propagation alike, which means a guard at the top of every state transition for a control nobody asked for. Note for whoever meets it: **Ardour's internal "solo safe" is not Pro Tools' Solo Safe.** Pro Tools' Solo Safe is Ardour's Solo Isolate, which is what was built. Reading Ardour's source with Pro Tools habits implements the wrong feature. |
| Dim, and solo boost | Rejected. Both are monitor section controls in Ardour (`monitor_processor.cc`), applied after the mix rather than inside the solo arithmetic, and a dim that attenuated the master output would be changing the mix rather than the monitoring, which is the opposite of what the word means. Their figures are recorded here so nobody has to look them up again: dim defaults to -12 dB over a -20 to 0 dB range, solo boost to 0 dB over 0 to +10 dB. |
| Momentary solo | Rejected for tonight, with the mechanism recorded because it is the part that is easy to get wrong. Ardour's `Session::prepare_momentary_solo` partitions every route into those currently soloed and those not, and the release sets each list back to where it was. It restores a **snapshot** rather than toggling, so a momentary solo cannot leave the session in a state it was not in before. If it is ever built, build it that way. |
| Saving the propagation counters the way Ardour does | Rejected deliberately. `SoloControl::get_state` writes `self-solo`, `soloed-by-upstream` and `soloed-by-downstream`, and restoring a derived quantity means a file can be internally inconsistent with its own routing: the project would load audibly wrong with nothing to say why. Only the user's two instructions are saved, and the closure is recomputed after load. That is the same discipline `WarpMap.h` already encodes by normalising on load. Ardour itself argues for this from the other end: it ships `SoloControl::clear_all_solo_state`, a repair path with user visible logging for leaked counters, which is the clearest possible statement that incremental counters leak in practice. |

### What was chosen, and why

**There was no way to hear one thing on its own.**

To listen to the vocal, the only route was to mute the other fifteen inserts by
hand and then remember which of them had already been muted before starting.
That is a daily mixing action everywhere else and it was one click elsewhere
against fifteen here, plus a state nobody can reliably restore. It is the
clearest case of the three the standing brief asks for at once: standard
everywhere, missing entirely, and far more actions here than anywhere else.

The reason it is a night's work rather than a button is the part nobody sees
until it is wrong. Muting everything that is not soloed also mutes the reverb
the soloed vocal is sending to, so soloing a vocal gives a dry vocal, and
soloing the reverb gives a reverb with nothing going into it. Every studio has
an answer to that and the answers differ.

### Where the behaviour came from

Primary sources throughout: Ardour's own git tree at `d66bd6ce` read directly,
Ardour's published manual, Avid's own Pro Tools user guide pages, Apple's own
Logic Pro manual, and Cockos' own wiki. Four of the eight questions asked have
a complete primary answer from Ardour only, and each gap is named below rather
than filled in with a plausible guess.

- **Propagation through sends, which is the whole difficulty.** Ardour is the
  only one of the four that publishes an algorithm, and it is complete.
  `Session::route_solo_changed` keeps two counters per route,
  `_soloed_by_others_upstream` and `_soloed_by_others_downstream`, and for every
  other route asks `i->feeds (route)` and `route->feeds (i)`, incrementing the
  downstream counter on things that feed the soloed route and the upstream
  counter on things it feeds. `Route::feeds` delegates to `GraphEdges::feeds`,
  which is recursive and does **not** filter on its own `via_sends_only` flag,
  so a send edge propagates exactly like a main output edge and propagation is
  over the transitive closure rather than immediate neighbours. **Both
  directions propagate, automatically, with no preference to turn it off.**
  Read the counter names twice: on route R, `_soloed_by_others_upstream` means
  something upstream of R is soloed. The counter is named for where the soloed
  route sits, not where R sits.
- **What that produces here.** Solo the vocal on insert 3 which sends to the
  reverb on insert 9, and 9 is soloed-by-upstream and stays audible, so the
  vocal is heard with its reverb. Solo the reverb instead and the vocal is
  soloed-by-downstream and stays audible, so the dry vocal is heard as well:
  Ardour's manual states that one outright, "Soloing a bus will also solo any
  tracks or busses which feed that bus". It surprises people and it is still
  the published rule.
- **The hardest case, which a naive implementation fails.** Vocal on 3 and
  guitar on 4 both send to the reverb on 9. Solo 9: all of 3, 4 and 9 are
  audible. Now also solo 3, and the guitar must go silent while the vocal and
  the reverb stay. Ardour gets there through `push_solo_upstream`, with its own
  comment: "When we solo 1 track out of N that feed a bus, that track will
  cause mod_solo_by_upstream (+1) to be called on the bus. The bus then needs
  to call mod_solo_by_downstream (-1) on all tracks that feed it." Computed as
  a closure rather than as counters, the same answer falls out of one rule: an
  insert is audible when it is soloed itself, or it is upstream of something
  soloed, or it is downstream of something soloed **and nothing soloed sits
  between them**. The test asserts the case directly.
- **Where the gain is applied, which is the half that is easy to miss.**
  Ardour applies it per output point rather than per route, in
  `Delivery::run`, and with its shipped defaults (`mute-affects-main-outs` and
  `mute-affects-post-fader` both true) an insert silenced by somebody else's
  solo has **both its main output and its post-fader sends** cut. Without the
  second half, a guitar silenced by a vocal solo still feeds the reverb bus,
  the reverb bus is audible because the vocal soloed it upstream, and the
  guitar is heard through the reverb. The sends here are post fader and are
  copied out of the insert's buffer after `processInsert` has applied the
  fader, so folding the solo gain into that one multiply cuts both at once,
  which is Ardour's default behaviour and not a shortcut. The test measures it
  rather than trusting it.
- **Solo isolate: three studios, one meaning, three names.** Ardour's manual:
  "When tracks or busses are soloed the isolated ones will not mute." Avid's
  own Solo Safe page: it "keeps a track audible when other tracks are soloed",
  and is "especially useful" on "Aux tracks, busses, and effects returns, so
  shared effects like reverb or delay aren't cut off when you solo a related
  track". Apple: "The channel strip does not mute when you solo another channel
  strip." All three mean exempt from being muted by somebody else's solo, and
  none of them means unsoloable. Avid's own caution is worth repeating in the
  tooltip: "Don't apply Solo Safe to every track, since that defeats the
  purpose of soloing." Called Solo Isolate here, following Ardour and Logic's
  description, with "solo safe" in the tooltip because that is the word two
  thirds of users will arrive with.
  That Pro Tools and Logic both need this control at all is the evidence that
  neither propagates: a studio that propagated upstream would not have to tell
  people to solo-safe their reverb returns.
- **The master needs no isolate of its own.** Every insert feeds insert 0, so
  insert 0 is downstream of everything and is soloed-by-upstream whenever
  anything is soloed at all. It falls out of the rule, and the test asserts it
  for every send matrix and every solo set, because the failure if it ever
  stops being true is that soloing produces silence.
- **Mute against solo, where Ardour's code and Ardour's manual contradict each
  other.** `MuteMaster::mute_gain_at` has two branches on
  `Config->get_solo_mute_override()`, which ships `false`, and in that default
  branch an explicit mute is tested first and wins: **a track that is both
  muted and soloed is silent.** Ardour's own mixer strip page says the
  opposite, "by default, Solo overrides Mute, i.e. if a track is both Soloed
  and Muted, it will play". The source is primary for behaviour and the config
  default and the arithmetic agree with each other, so the manual sentence is
  treated as stale. No primary source was reachable for Pro Tools, Logic or
  Reaper on this question, so **there is no majority to follow and the default
  here is a choice**: explicit mute wins, because a mute somebody set by hand
  is an instruction and a solo elsewhere is a temporary audition. It is a
  preference, and the arithmetic of both branches is implemented exactly,
  including the asymmetry that is the first thing a paraphrase loses: in the
  override branch the test is `_soloed_by_self`, not `soloed()`, so self solo
  beats an explicit mute and being soloed by propagation does not.
- **The solo mute gain.** Ardour's `solo-mute-gain` is a linear coefficient
  defaulting to `0.0`, full mute, presented as a fader labelled "Solo-in-place
  mute cut (dB)". Its manual: "Setting this level to -inf dB will mean that
  other tracks will not be heard at all". Full mute is the default here too,
  and the setting is in decibels with silence at the bottom of the range, the
  way the metronome level already is.
- **Additive, not exclusive.** Ardour ships `exclusive-solo = false` and Logic
  documents additive solo with Option-click for exclusive. Additive is the
  default here, exclusive is a preference, and a modifier click does the
  opposite of whatever the preference says, through the same code path so the
  two cannot drift apart. Following Logic, a modifier click on a solo that is
  already on clears every solo, which is the cheapest possible way out.
- **The three state solo button is not cosmetic.** Ardour's
  `RouteUI::solo_active_state` returns explicit, implicit or off, from
  `self_soloed()` and `soloed_by_others()`. Without it, soloing a vocal lights
  nothing on the reverb bus and there is no way to tell why the reverb is
  still audible. Ardour also draws the mute button outlined on anything muted
  by somebody else's solo (`show-solo-mutes`, defaulting on) and Logic flashes
  it; both are the same idea and both are built here.
- **The indicator that lights when anything is soloed, and clears it.**
  Ardour's is internally called "rude solo", bound to `Main/cancel-solo`, with
  the tooltip "When active, something is soloed. Click to de-solo
  everything". It is the cheapest defence against the commonest error with
  solo, which is leaving one on and then wondering where the mix went.

### The two decisions taken without a primary source, recorded as decisions

**Whether a bounce honours solo. Nobody's reachable documentation says.** The
pages that would have answered it were refused or unreachable: Avid's
"Bouncing a Mix", Ardour's export dialog page, and the bodies of both the
Reaper and Pro Tools reference guides, whose relevant page numbers are written
into the research record so a later night can go straight to them. What could
be established is from Ardour's source rather than its prose: there is not one
occurrence of "solo" anywhere in its export path, and the solo gain lives in
`Delivery::run` which is on the graph the export runs, so an Ardour export
taken while something is soloed exports the soloed material.

Solo is honoured in `renderOffline` here, identically to playback, for the
reason `CLAUDE.md` gives: two code paths over one graph, and anything added to
one and not the other makes a bounce sound different from what was heard. The
precedent for diverging exists and is narrow, `exportingFullyCompensated`, and
it diverges because the live behaviour is a monitoring convenience with a named
reason; the difference is that honouring solo in a render cannot produce a
wrong file, only a short one, and it is audible immediately. Printing a stem by
soloing it is also an ordinary thing to want. What makes the choice safe either
way is that the export says so in words before it starts, the way the solo
indicator does for playback.

**That solo is not undoable.** It follows the mixer state that is already
there rather than being decided afresh: volume, pan and mute all live in the
engine and are saved and loaded by `ProjectIO`, and none of them is in
`Project::capture`. Solo is a listening state, and an undo history with
fourteen solo clicks in it is an undo history nobody can use. So of
`CLAUDE.md`'s four things it needs three: saved in `save`, restored in `load`,
reset in `clearAll`.

### What could not be sourced, named as such

- **Pro Tools' solo latch modes** (Latch, X-OR, Momentary), their default, and
  what Bounce To Disk respects. Everything reported about Pro Tools comes from
  two short Avid web pages. The Digidesign 6.4 guide that was reachable is 22
  years old and truncated before the relevant chapters; the places to look are
  "Soloing and Muting Tracks" p.100, "Bounce to Disk" p.482 and "Bounce
  Options" p.483.
- **Logic's behaviour when a soloed track's send feeds an aux.** No Apple page
  found says.
- **Reaper**, on almost everything. Its user guide body was unreachable and the
  one Cockos wiki page that was readable shows that its vocabulary is inverted
  relative to everybody else's: in Reaper, "in-place" solo means the track's
  own fader output with its sends **excluded**, and the Alt variant is the one
  that keeps the reverb. That is the opposite of what solo in place means in
  Ardour and Pro Tools. **Reaper's wording was deliberately not borrowed.** The
  sections to read are 5.2 "Track Control Modifiers" p.92, 5.3 "Mute/Solo In
  Front" p.93, and 21.3 "Rendering A Project" p.403.

### What the audit changed, and what it did not

A separate agent audited the whole change on the assumption it was wrong. The
arithmetic survived: it mutation tested `SoloGraph.h` independently, got the
same six survivors the header documents, and could not break the closure. Every
fault it found was in the engine, the interface or the comments, which is worth
recording because that is where the author had stopped looking.

Two were real behaviour:

- **Solo stepped the gain instead of ramping it.** One press moves up to
  fifteen inserts from unity to zero, and a flat multiply puts the step at
  whatever the signal happened to be on each of them. A step falls at only
  6 dB per octave, so a mix at -10 dBFS gives a broadband click at -10 dBFS
  into the master chain, on every press and every release, and worst of all on
  an exclusive solo where one gain rises as fourteen fall. **The research
  recorded where Ardour applies its figure and never asked how it gets
  there:** `Delivery::run` interpolates towards it across the buffer, and that
  interpolation is what keeps Ardour's mute and solo quiet. Ramped over five
  milliseconds now, which is the figure and the argument `mixMonitorInput`
  already had. Mute is de-clicked by the same change, having been a flat
  `? 0.0f :` since long before this.
  The ramps are latched to their target rather than ramped at the start of a
  render and when a device opens: solo cannot change during a bounce, so the
  only thing a ramp could do there is put a five millisecond fade on the front
  of the file.
- **A send turned all the way down still counted as a route, and the header
  claimed the opposite rule two screens earlier.** `isLegalSendTarget`'s
  comment said it had to agree exactly with `routeSends`, which skips a send
  whose level is at or below zero, and `levelAffectsRouting = false` then
  declared that it did not. The stated failure was live and the audit measured
  it: a vocal whose send is pointed at a reverb with the amount knob turned
  down to nothing, which is the ordinary way to take the reverb off that
  vocal, made soloing the reverb play the full level dry vocal, with the
  vocal's own strip lighting its solo button by propagation to explain why.
  The level is part of the closure now.
  **This is a departure from Ardour and is recorded as one.** Its
  `Route::feeds` is about connections and takes no account of a send's gain,
  so an Ardour send turned down is still a propagation edge. Its mixer is
  built differently: a send there is a processor that is present or absent,
  where here a send is a destination and an amount and the amount at nothing
  is how somebody switches it off. The price is that the closure now depends
  on a continuous control, which `setSend` already recomputes on.
  The test could not see it: every generated send had a destination and a
  level that agreed about whether it existed, so one legal send in eight is
  now turned all the way down on purpose.

Three were the interface:

- The mute button's new colour **fired on the opposite state.** That button's
  toggle state is inverted, true meaning not muted, so setting its off colour
  from `mutedByOthersSoloing` lit a hand muted strip whenever anything
  anywhere was soloed and never lit the strip the colour was for. The outline
  in `paint` was doing the job correctly all along, so the colour is gone and
  the outline now also requires that the strip is not muted by hand, the way
  Ardour's `update_mute_display` lets an explicit mute win.
- **Mute and solo side by side left each of them 13 pixels of text.** A strip
  is 66 pixels wide and 56 after its margins, and JUCE indents button text by
  half the corner size at each end, so a 27 pixel button at a 12 pixel font is
  a smear. Stacked now, full width each, with the strip 20 pixels taller so it
  did not come out of the fader's travel.
- **"Reset volume and pan" cleared solo isolate without saying so.** That is
  the one setting here whose loss is silent and deferred: reset a reverb
  return's fader and the next time somebody solos a vocal the return is cut
  and the vocal is dry. The item says "Reset this strip" now.

Two were comments that said something other than what the code did, which is
the fault `CLAUDE.md` records twice for `updateLatency` and which this change
managed twice more:

- The header's note on its own redundant guards **named the wrong pair and
  gave the wrong failure.** It claimed `soloedAt`'s guard and the propagation
  scan's bound were the dangerous pair and that removing both would silence
  the mixer. The audit removed them and measured the opposite: every solo
  becomes a no-op. The dangerous pair is `soloedAt`'s guard with the
  `anySoloed` loop's bound, which does silence the mixer. Both are written out
  now, with what each actually does.
- `PreferencesActions::setSoloSettings` **listed its two bools in the wrong
  order.** A `std::function`'s parameters have no names, so the comment was
  the only documentation a second caller would have, and following it swaps
  two flags silently: exclusive solo stops clearing the other strips and a
  strip that is both muted and soloed becomes audible, with nothing erroring.

And one smaller thing fixed on the way: a right click on the solo button both
soloed the strip and opened its menu, because `juce::Button` does not filter on
which mouse button. Pre-existing for the mute and arm buttons, but the solo
tooltip sends people there to right click for Solo isolate, so it is now on a
path the feature advertises.

### Found by the audit and deliberately not fixed

Each of these is real, each is recorded so a later night does not have to
rediscover it, and none of them is this change's to fix.

- **A mute or solo click during a bounce freezes the interface.** All six of
  the new setters take the graph lock, and `renderOffline` holds it from before
  the first block to after the last. `juce::SpinLock` spins and then yields, so
  one click during a ten minute stem export is a ten minute "not responding"
  window burning a core against the render thread. This is a pre-existing
  hazard reaching one more control: `setSend`, `setFx` and `setInsertInput`
  already block the same way, and `renderOffline`'s own comment names it as
  the reason `setPanLaw` does not take the lock. Mute was lock free before
  this change, so that one is a regression, and it is the control somebody is
  most likely to touch.
  The fix that would cover the whole family is for the render to drop and
  retake the lock between blocks, and that is not small: the reason it holds
  the lock throughout is so that one bounce is mixed under one graph and one
  pan law, which is the guarantee this studio exists to keep. A night of its
  own, and the answer may be to publish each instruction and recompute on the
  message thread's next turn rather than to loosen the render.
- **The top bar was already over subscribed before solo was added.** The
  audit measured it: at the default window of 1500 pixels the left hand chain
  needs more room than it has, the tabs are already clipped, and the Mix
  report tab is the one with no keyboard shortcut to reach it instead. Solo's
  indicator was moved into the right hand chain, which is laid out first and
  therefore cannot be the thing that falls off the end, and made narrower, but
  40 pixels is still 40 pixels off a bar that did not have them. The bar wants
  a real answer: a smaller set of always visible controls, an overflow menu,
  or the view tabs moved somewhere else.
- **Solo silences live input monitoring, and the meters with it.** Four
  microphones on four inserts with monitoring on, solo a guide part on a fifth,
  and all four players lose their headphone feed. Nothing is lost from the
  take, because the recorders tap the input before the mixer, but the players
  stop hearing themselves. This is what solo in place means and Ardour does the
  same, so it is modelled rather than overlooked, and the documented escape is
  solo isolate, which is exactly what Avid describes Solo Safe being for.
  Recorded rather than fixed because exempting a monitored insert would make
  solo useless while tracking, which is when a studio is most likely to have
  several of them: soloing the guide would leave all four microphones in.
  `LatencyGraph::Insert::monitored` exists for the compensation exemption and
  is deliberately not reused here. The meters going dead with it is a separate
  consequence of the same cause: this studio measures peak after the fader and
  Ardour's meter point sits before the delivery that applies the solo gain.
- **The partial solo mute cut is applied twice on a path through a send.** At
  the default the cut is exactly zero and the question does not arise. Set it
  to something in between and a signal reaching the master through a send is
  cut at its source and again at the bus, so it arrives at the square of the
  cut while the dry path arrives at the cut: at -12 dB the reverb returns of
  the held material are 12 dB below where they sit in the full mix, so they
  collapse rather than coming down with it. Ardour does the same for the same
  structural reason, its gain being per output point too. Fixing it means a
  second figure per insert, one for its own output and one for what passes
  through it, which is a feature rather than a tidy up. The test measures the
  number now rather than leaving it to be discovered.
- **The check against Ardour's algorithm is not independent of this project's
  reading of it.** The model in `SoloGraphTest.cpp` is a transcription of the
  quotes in this entry, which came from a sparse clone of Ardour at `d66bd6ce`.
  If that reading of `route_solo_changed` is wrong then the closure and the
  model are wrong together, and ten thousand agreeing solo sets prove only that
  they agree. The audit tried to re-verify it from the web and the fetch was
  refused. Worth doing from a fresh clone on a night that has one.

### Where the arithmetic lives

`SoloGraph.h`, no JUCE, so the whole closure is tested. It takes the send
matrix, each insert's mute, self solo and isolate, and the three settings, and
returns one gain per insert. The engine keeps only what needs JUCE: reading the
routing out of the atomics and multiplying one number into the fader gain that
`processInsert` already applies, which is also what carries it into the sends
and into the export without a second implementation.

Recomputed from scratch on every change rather than kept as counters. Ardour
maintains counters incrementally because its graph is arbitrary and its routes
come and go; this one is sixteen fixed inserts with an acyclic send matrix, so
a whole recompute is trivial and it makes the hardest bug in the feature, a
leaked counter, structurally impossible. The recompute hangs off the end of
`updateLatency`, which is the one function every path that changes a signal
route already has to call under the graph lock. A second list of call sites to
keep in step is a list that will fall out of step.

---

## 2026-10-15: clip fades, and the de-click fade every edge should already have had

### What was looked at

| Candidate | Verdict |
|---|---|
| **Clip fade in and fade out, crossfades by overlap, and an automatic de-click fade at every clip edge** | **Chosen.** Deferred on 2026-10-08 and twice on 2026-10-13, each time as "the largest real gap". This is the night that does only that. |
| Fader law | Deferred again, and still a real gap: `processInsert` turns the fader position into gain with `(v / 0.8)^2`, where every studio tapers in decibels. Not chosen because it changes the meaning of every stored `volume` in every saved project, so it wants a night with the version translation as its main subject rather than as a side effect of something else. |
| MIDI record alignment | Deferred again, for the reason the last two nights gave rather than a new one: the figure is easy and knowing whether the keyboard is being played live or a pattern is being bounced off the grid is not. |
| A tuner | Deferred. Still wants a published pitch detector and a window. |
| Punch in and punch out recording | Deferred, and note that it now has a dependency worth having: Pro Tools' punch recording works by auto crossfading at the drop-in and drop-out points, which is exactly the machinery this night builds. Cheaper after tonight than before it. |
| A separate crossfade object, the way Pro Tools models one | Rejected, on the research. Reaper and Ableton both make a crossfade an overlap of two clips' own fades, and `renderAudioClips` already sums overlapping clips on a track into the same insert, so a fade out meeting a fade in **is** a crossfade with no new object, no new render path and no new file format entity. Ardour makes the same point from the other direction: its manual says "It is important to understand that region fades *are* crossfades." |
| Ardour's `FadeFast` and `FadeSlow` shapes (decibel-linear) | Rejected for tonight and worth recording. Both are real and Ardour recommends them as general-purpose fades, but each needs a floor convention to exist at all (Ardour's are built by dropping 60 dB and 80 dB over the fade and then jumping to -140 dB at the end), and that floor is an arbitrary constant rather than a specification. Their crossfade partners are built as `sqrt(1 - g^2)` and are therefore non-symmetric, which Ardour's own manual says limits their use. Three shapes with exact closed forms beat five where two carry a made-up number. |
| Ableton's signal-dependent edge fade length | Rejected as unimplementable from the documentation. Live documents the fade as "signal-dependent" and "ranges from 0-4 milliseconds" but does not publish how the length is chosen from the signal. A guess at it would be precisely the plausible-and-wrong failure this project keeps a record of, so the figure is used as a fixed length and the fact that Live varies it is recorded here instead. |
| Reaper's v7.81 continuous curvature and S parameters | Rejected: no published mapping to a curve. Reaper's seven numbered shapes have no published formulas and no published names either, beyond `0 = linear`. Anything giving formulas for them is reverse engineering. |

### What was chosen, and why

**Cutting a clip anywhere but at a zero crossing clicks, and there was nothing
at all to stop it.**

`Clip` carried `gainDb` and `muted` and no fade of any kind. The only per-clip
gain shape in the engine was `CompFade`, which exists for the joins *inside* a
take folder, so a comp was crossfaded and the clip's own two edges were a hard
cut. Every studio researched applies a short fade at a clip boundary for exactly
this reason, and all four of them give the user handles to make it a long one.

For somebody recording guitar and vocals this is daily, three times over:

- A take trimmed to its first note starts on whatever sample the trim landed on.
  Full scale on the first sample is a step, and a step's spectrum falls at only
  6 dB per octave, so it puts energy right across the audible band. That is the
  click.
- Splicing the good half of one take onto the good half of another means two
  edges meeting, and nothing was ramping either of them.
- Fading out the end of a song had to be drawn as an automation curve on the
  insert, which is four or five actions where every other studio is one drag.

### Where the behaviour came from

Read from the primary source in every case: Ardour's own git tree at
`1d826f3`, Cockos' own extension SDK header, Ableton's own manual, Avid's own
help pages, Apple's own Logic manual, and Harris 1978 for the one piece of
arithmetic that is actually published.

- **The two crossfade laws, and which material each is for.** Avid states the
  rule in both directions on its Fades Dialog Link Settings page: **Equal
  Power** is "Recommended for material that is not phase coherent", and **Equal
  Gain** is "Recommended for material that is phase-coherent or nearly
  phase-coherent", each with the other's failure named (equal gain dips on
  uncorrelated material, equal power clips on correlated material). The
  criterion Avid uses is **phase coherence**, not "same file", which is the
  right criterion and is what the header's comment says. Ardour's manual states
  the same rule with the numbers: linear is "the correct crossfade to use with
  highly-correlated signals" and its sides are "each -6dB attenuated at the
  midpoint", while constant power is "the correct crossfade to use when splicing
  audio in the general (uncorrelated) case" and its sides "each reach -3dB at
  the midpoint". Apple is the third independent statement, naming **EqP (Equal
  Power Crossfade)** and giving the reason as minimising "volume dips between
  audio regions".
- **The arithmetic of each.** Equal gain is `g_in = x`, `g_out = 1 - x`, which
  is Ardour's `FadeLinear`: two breakpoints and, for the layer underneath,
  literally `1 - gain` (`libs/ardour/audioregion.cc`). Equal power is
  `g_in = sin(x*pi/2)`, `g_out = cos(x*pi/2)`, which is Ardour's
  `FadeConstantPower`, whose own source comment reads "constant-power fades use
  a sin/cos relationship". Midpoints are 0.5 and 0.5 summing to unity gain, and
  0.70710678 and 0.70710678 summing to unity power. Nothing here was fitted.
- **The default shape is equal power**, following Ardour's
  `default-fade-shape`, which is `FadeConstantPower` in
  `rc_configuration_vars.inc.h` and is what both its region fades and its
  crossfades get. Recorded as a disagreement rather than a consensus: Pro
  Tools' documented default is Preset Curve 4, which Avid says "fades in with a
  linear fade curve. This is the default curve", and Avid's user guide calls its
  Default Fade Out "standard equal gain". Reaper's shipped default is not
  documented at all.
- **The de-click length.** Ableton documents 4 ms, twice and in two places: the
  Clip View section says the automatic edge fade "ranges from 0-4 milliseconds",
  and the Arrangement View section says a deleted fade handle resets to "a
  default length of 4 ms" and that with **Create Fades on Clip Edges** enabled
  "adjacent audio clips will automatically get 4 ms crossfades". Ardour's is 64
  samples, a bare literal appearing four times in its tree with no named
  constant, which is 1.45 ms at 44.1 kHz. Avid suggests 10 ms for its punch
  crossfade. 4 ms is the middle of that range, is the one figure a manufacturer
  publishes as a number of milliseconds rather than a number of samples, and is
  the default here.
- **Ardour's 64 samples is deliberately not copied**, and the reason is the one
  `CLAUDE.md` already records for the K-weighting filters. A fixed sample count
  gets shorter in time as the sample rate rises: 1.45 ms at 44.1 kHz and
  0.33 ms at 192 kHz, which is backwards, because what the fade has to do is
  set by the audible band and not by the converter. The length here is a time.
- **The de-click shape is the raised cosine, and this is the one piece of
  arithmetic in the feature that is derived rather than quoted.** The published
  part is Harris, "On the Use of Windows for Harmonic Analysis with the Discrete
  Fourier Transform", Proc. IEEE 66(1), January 1978, whose Table I gives the
  sidelobe fall off against the order of the boundary discontinuity: -6 dB per
  octave for the rectangle, -12 for the triangle, "reflecting the discontinuity
  of the window residing in the first derivative", and -18 for Hann, where "the
  discontinuity of this window resides in the second derivative". Nuttall, NUSC
  TR 6239, 1980, states the same as an asymptotic expansion. Applying that to a
  fade's two joins separately, which is the step that is easy to get wrong
  because the two joins are not the same: a linear ramp leaves a slope
  discontinuity at both ends and so decays at 12 dB per octave at both;
  `sin(x*pi/2)` has zero slope where it meets unity but not where it meets
  silence, so it is 18 dB per octave at one end and 12 at the other; the raised
  cosine `(1 - cos(x*pi))/2` has zero slope at both ends and is 18 dB per
  octave at both. So the shape a de-click fade wants is the raised cosine, and
  the project default shape is only half as good as it at the end that matters
  most. **Ardour uses its default shape for its de-click and therefore gets
  12 dB per octave at the silent end.** The test measures the decay rather than
  asserting the citation.
- **The raised cosine is an equal gain law, which is the second reason to use
  it for the de-click and was not obvious.** `g(x) + g(1-x) = 1` exactly, by
  the cosine's own symmetry. So when two clips of the same performance abut and
  their automatic fades overlap, the join is phase coherent material through an
  equal gain crossfade, which is exactly what Avid's rule asks for. Both
  arguments point at the same shape, from different directions.
- **There is no established default for a fade from silence**, and the comment
  says so rather than implying an authority. Ardour uses constant power and
  publishes a reason, but the reason is about Ardour's data model rather than
  about sound: in a layered model every region fade is a crossfade against
  whatever is beneath, and "If a region doesn't have any region(s) under it,
  then the region is crossfaded to silence". Pro Tools uses linear and publishes
  no reason. Logic treats equal power as belonging to crossfades specifically.
  The theoretical argument does not transfer either: equal power exists to hold
  the sum of two uncorrelated powers constant, and against silence there is no
  second term to hold anything constant with. Recorded as a genuine gap.
- **A fade length is a time, anchored to its edge, and is never rescaled.**
  Every studio checked stores it in absolute time and not one stores it as a
  fraction of the clip: Ardour's fade is an `Evoral::ControlList` built with
  `Temporal::AudioTime` so it does not move with tempo, and Reaper's
  `D_FADEINLEN` is documented as "item manual fadein length in seconds". A
  fraction is the plausible-and-wrong choice, and wrong in a way that would be
  hard to diagnose: trimming a clip would change how its fade sounds, and
  stretching one would change its de-click length.
- **Reaper's data model is copied for the one thing it does better than the
  others.** Its SDK header carries `D_FADEINLEN` and `D_FADEOUTLEN` for the
  user's fade and **separately** `D_FADEINLEN_AUTO` and `D_FADEOUTLEN_AUTO`,
  "in seconds, 0 means no auto-fadein". Two quantities per end rather than one,
  so the studio's de-click fade and the user's fade coexist instead of one
  overwriting the other. Here that is a stored length per end which is zero
  until somebody drags it, and a project-wide automatic length that fills in
  every edge that is still zero. Setting the automatic length to zero disables
  it, which is Reaper's own convention for the same field.
- **Overlapping fades are refused, not allowed to double up.** Ableton's manual
  states it flatly: "A clip's start and end fades cannot overlap each other."
  Worth copying deliberately, because Ardour's library layer clamps each fade
  independently and then lets `read_from_sources` apply both to the same samples
  when they meet, which is an implementation gap rather than a behaviour.

### What was built

`Source/ClipFade.h`, with no JUCE in it: the three shapes as closed forms, the
resolution of what a clip asks for into what its length can actually carry, and
the gain at a beat. The falling half of every shape is the rising half read
backwards rather than a second formula, which is what makes the crossfade
identities exact instead of approximate: `sin((1-u)*pi/2)` is `cos(u*pi/2)` by
the identity and not by rounding.

`Clip` gains four fields, `fadeIn`, `fadeOut` and a shape for each. `Project`
gains `autoFadeSeconds`. The engine's `AudioClipRT` gains the window, built
once per clip in `pushArrangement` and copied into every entry a clip produces,
so a take folder's outer edges fade while its internal comp joins keep the
crossfades they already had, and the two multiply rather than fight. The one
line in `renderAudioClips` that applies `compFadeGain` now applies both, which
is the whole of the engine change: `renderOffline` calls `renderAudioClips`
over the same snapshot, so the export gets fades for free and cannot disagree
with what was heard.

`Project::split` clears the fade at each new inner edge, so the two halves of a
cut are de-clicked by the automatic fade while keeping whatever the user had
drawn at the two outer edges. That is Ardour's behaviour on a split, where a
newly created inner edge always gets a default fade.

### Deliberately left for later

- **Fade handles require the clip to be selected**, the same rule the warp
  markers and the automation handles already follow, because the top of an
  unselected clip is the grab handle for moving it. Reaper and Ardour both show
  fade handles on hover without a selection. One extra click, and worth
  revisiting when there is a reason to touch the playlist's hit testing again.
- **A curve amount, rather than a choice of three shapes.** Reaper from 7.81
  and Logic both expose a continuous curvature, which is a better control than
  a menu. Neither publishes how the number maps to a curve, so doing it here
  means inventing a mapping, which is worth doing deliberately and saying so,
  not as a side effect of this.
- **Ardour's second trim mode**, where dragging from the fade area keeps the
  timeline position at which the fade reaches unity fixed and changes the
  fade's length to suit, rather than moving the fade with the edge. Ardour
  ships both and picks between them by where the drag started. The one built
  here is move-with-the-edge, which is the common case.
- **Refusing a crossfade that has no source material behind the join.** Avid
  states the rule, that "a crossfade cannot be performed on clips that do not
  contain audio material beyond their clip boundaries", and Ardour implements it
  in `verify_xfade_bounds`. It does not arise here yet, because a crossfade is
  an overlap of two clips that are each already only as long as their own
  audio, so there is nothing to fake. It becomes real the moment there is a
  gesture that creates an overlap by dragging a clip's edge out past its source.
- **A fade on a MIDI clip.** Not a thing in any studio checked, and the
  arithmetic would have to act on velocity rather than on gain.
- **The automatic fade is not in the undo history.** It is saved, loaded and
  reset by `clearAll`, but a de-click length is a property of how the studio
  joins audio rather than of the arrangement, and nobody pressing Ctrl+Z after
  drawing a note expects it to move. Recorded because `CLAUDE.md` asks for four
  things and this field deliberately takes three.
- **The automatic de-click length has no home of its own.** It is on the fade
  handle's own menu, marked as applying to the whole project, because that is
  the only place a control for it could go without building a page for it.
  Reaper keeps the same figure in its project defaults and that is where it
  belongs eventually.
- **A real crossfade still has to be made by overlapping two clips.** Dragging
  a fade at an exact join makes a dip rather than a crossfade, because the two
  ramps are consecutive rather than simultaneous, which is inherent to clips
  that do not overlap. Overlapping them works, and the renderer sums them
  correctly, so the gesture exists; what is missing is the one that does it for
  you. Ableton's "adjacent audio clips will automatically get 4 ms crossfades"
  is that gesture, and it works by extending the material rather than by
  shaping it. Doing the same here means a clip's played span reaching past its
  own edges in the snapshot, which touches the warp and stretch geometry, and
  is a night of its own.

### What the audit found

Mutation testing of `ClipFade.h` tried 61 deliberate breakages and the test
caught 54. Eleven survived the first round: six were real gaps and the test was
strengthened until each was caught, and five were argued as no-ops. Eleven
further mutations aimed at the parts that round left thin found one more real
gap and two more no-ops. The seven survivors are all genuine no-ops and the
arguments are in the mutation report; two are worth having in the header and
are now there.

Three of the real gaps are worth recording because of their shape rather than
their fix:

1. **A wrong curve could pass.** `sqrt(x)` is monotone from nothing to one,
   sits exactly at one over root two at its midpoint, and satisfies
   `g(x)^2 + g(1-x)^2 = 1` **exactly**, because `x + (1 - x) = 1`. So it
   answered every property the test asserted about equal power, and was caught
   only by the comparison against the closed form, which is a restatement of
   the implementation. That is precisely the fault `CLAUDE.md` records for
   `PanLaw`'s first test. The test now checks the thing the header's table is
   actually about: which join of each ramp is flat and which is not, measured
   as a one sided difference at each end. `sqrt`'s unity end reads 0.5 whatever
   the step, so it fails on a property now rather than on a formula.
2. **`numShapes` could be changed from three to two and nothing failed**,
   because every identifier check was parameterised by it and was therefore
   checking the count against itself. Meanwhile `ProjectIO` writes the
   S-curve's id as 2 and reads it back through `shapeFromId`, which would no
   longer recognise it, so every saved S-curve fade would have loaded as
   linear: a fade that is quietly the wrong shape, which is the exact failure
   the named fallback exists to avoid.
3. **One clamp of a symmetric looking pair is load bearing and the other is
   provably dead.** `windowFor` raises the fade out's start to the fade in's
   end, and that line is not defensive: the engine reaches it through a
   division by the tempo and a multiplication back by it, and the pair lands
   one unit in the last place past the span on about five per cent of real
   positions, which is an overlap at ordinary places such as a four beat clip
   forty minutes into a session. Its sibling, the clamp of the same value to
   the clip's start, can never bind. Both are documented now, the way `PanLaw.h`
   documents its own survivor.

A separate agent then audited the whole change on the assumption it was wrong,
and found three things that would have shipped. The first two are ordinary
wiring faults; the third is a design error and is the useful one.

1. **A fade edit was invisible to the undo history and to the dirty flag.**
   `Project::sameClip` compares every field of a clip that counts as an edit,
   and the four new ones were not in it. `capture` copied them, so they would
   have ridden in the history, but `commit` never got that far: it saw no
   change, returned false, and `markDirty` was never called. So drawing a fade
   and closing the window lost it with no prompt and no asterisk, and a single
   Ctrl+Z afterwards removed the fade **and** undid the previous unrelated edit
   as one step, with the fade unrecoverable. This is the "captured for undo"
   leg of the four things `CLAUDE.md` asks of new state, failed in the one way
   that leaves everything looking right.
2. **Choosing a fade shape from the menu never reached the engine.** The menu's
   callback committed the edit and repainted, and `pushArrangement` is reached
   only from `project.changed()`. So the painted curve changed shape
   immediately and the engine carried on with the old one, and because of the
   fault above nothing marked the project dirty either, so there was no cue at
   all that the picture and the sound had parted company.
3. **An automatic fade at an abutting join is a hole, not a crossfade.** This
   is the one that matters and the research was quoted and then not followed.
   Clips here do not overlap unless somebody overlaps them, so two abutting
   clips' automatic fades are **consecutive rather than simultaneous**: the
   first ramps down to nothing across its last four milliseconds and the
   second ramps up from nothing across its first four, and the composite gain
   passes through exactly zero at the join. `Project::split` and
   `Project::duplicate` both produce exactly that join, and at a join where the
   waveform was continuous that is strictly worse than the hard cut it
   replaced. Measured by the audit at 48 kHz: up to 0.99 of full scale of
   error, 11 dB below the signal at 1 kHz, and three milliseconds of full scale
   silence removed at every cut. Slicing a drum loop on the grid put a hole on
   every slice; duplicating a one bar loop put one on every bar line.

   The equal gain identity the header leans on, `g(x) + g(1-x) = 1`, is a
   statement about two ramps across the **same** span. Abutting ramps are
   across adjacent spans and the identity says nothing about them. Ableton does
   get a crossfade at an abutting join, which is what its own "adjacent audio
   clips will automatically get 4 ms crossfades" describes, but it gets it by
   overlapping the material: the overlap does the work and the fades only shape
   it. Reading that line as licence to shape two abutting clips was the error.

   The fix is `ClipFade::Request::autoIn` and `autoOut`, set false by
   `Project::edgeIsJoined` wherever another clip on the same track meets this
   one to within a millionth of a beat, which at any sensible tempo is well
   under one sample. An exact join was made exact by the studio rather than by
   hand, so it is the only case where the audio either side is continuous; a
   clip dragged to within a millisecond of another has a real gap and a real
   step and still wants de-clicking. A **drawn** fade is never declined,
   because somebody who drags a handle at a join means it. `ClipFadeTest` now
   asserts the join both ways round: declined, the composite gain is bit for
   bit unity across it, and granted, it reaches zero. Asserting only the first
   would have been passed by a studio with no automatic fade at all.

The audit also found four comments that said something other than what the code
did, which is the fault `CLAUDE.md` records twice for `updateLatency`, and all
four are fixed: the claim that abutting clips' fades overlap, as above; the
claim that an overflowing pair is split in the ratio asked for, when it is
split in the ratio of the two clamped lengths, which is the better rule and now
has the comment that says so; the save side's reason for writing the automatic
length unconditionally, which cannot be the reason, because an older build
cannot write that attribute at all and it is the load side's default that makes
an old project open sensibly; and the test's reason for zero padding the
transform, which is about where the bins land rather than about periodicity.
`"only half as good"` was wrong arithmetic too: 12 against 18 decibels an
octave is a third worse, not half, and it is the order that halves rather than
the figure.

Four smaller things in the playlist, all fixed: the fade band had no minimum
clip width guard, so on a clip narrower than about ten pixels the whole top of
the body became the fade in handle and the trim gesture was unreachable; the
drag had no grab offset, so the fade jumped by up to the five pixel hit
tolerance at the first mouse move; `paintFades` measured its curve from the
clipped left edge rather than from the ramp's own start, so a clip scrolled
half off the left drew its fade in restarted at the viewport edge at the wrong
shape; and double clicking a fade handle dropped a warp marker underneath it.

Checked and found sound, so that nobody re-checks them: the export path (one
application site, `renderOffline` calls the same `renderAudioClips` over the
same snapshot, both `AudioClipRT` construction sites set the window, and an
export starting inside a fade reads the correct partial gain); the "no sample
is ever in both ramps" invariant, over four million random geometries under
sanitizers with lengths from 1e-300 to 1e6 beats; the audio thread, where
`gainAt` allocates nothing, every member of the window has a default
initialiser so an unfilled one reads unity, and a non-finite beat returns unity
before any arithmetic; the warped, stretched, mid-render and folder cases, where
`lengthBeats * 60 / bpm` is the output span in every one; and the de-click
derivation itself, re-derived independently and confirmed, with the measurement
shown not to be reading its own construction or a numeric floor (a deliberate
quantiser collapses the slope to -14.91 and the scatter to 5.56 dB, which is
why the scatter bound was tightened from 6 dB to 2.5).

One gap in the measurement is worth naming rather than fixing: `decayPerOctave`
ramps both ends of its block, so it can only see the worse of the two joins.
Swapping the two columns of the header's table leaves every printed number
unchanged. The join order is now asserted separately, as a one sided difference
at each end, which is what closes it.

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
