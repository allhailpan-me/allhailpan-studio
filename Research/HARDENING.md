# Hardening log

What each night audited, how hard it tried to break it, what it found, and
what it judged to be a genuine no-op rather than a gap. A target recorded as
clean here does not need re-auditing unless the code has changed since.
Newest first.

---

## 2026-10-22: the state variable filter (in progress)

Auditing `Source/Fx/Svf.h`, the two pole state variable filter every effect in
the studio is built on, picked up from where the night of 2026-10-21 left it:
that night fixed two faults in `Fx/OnePole.h` and recorded that `Svf.h` "has
the identical shape" and "wants its own night rather than being dragged into
this one".

Both faults are confirmed present. Still to do: establish reachability through
each of the five things that hold an `Svf`, fix, strengthen
`Tests/SvfTest.cpp`, and mutation test.

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
