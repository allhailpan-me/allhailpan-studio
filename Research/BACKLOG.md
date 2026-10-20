# Research backlog

A running record of what each night looked at, what it built, what it rejected,
and why. Rejected ideas stay listed so a later night does not revisit them
blindly. Newest first.

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
