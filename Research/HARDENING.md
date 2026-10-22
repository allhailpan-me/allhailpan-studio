# Hardening log

What each night audited, how hard it tried to break it, what it found, and
what it judged to be a genuine no-op rather than a gap. A target recorded as
clean here does not need re-auditing unless the code has changed since.
Newest first.

---

## 2026-10-22: the state variable filter

### What was looked at

`Source/Fx/Svf.h`, 342 lines, no JUCE: the two pole state variable filter
every effect in the studio is built on, and the thing `PassFilter`,
`BellMatch`, `LowPassGate` and the instrument's voice all run through. It was
not chosen for having no test. It had `Tests/SvfTest.cpp`, 1083 lines, and a
good one. It was chosen because the night of 2026-10-21 fixed two faults in
`Fx/OnePole.h` and recorded that this file "has the identical shape", in the
same two places, and "wants its own night rather than being dragged into this
one". Both were still there.

### Two faults, the same two, and the bigger one is a blast of noise

**1. A corner set at one sample rate and then read at a lower one puts the
poles outside the unit circle.**

`set` clamped the cutoff against `sampleRate * 0.49` using the rate as it
stood at that moment, and stored the clamped value. `setSampleRate` then
changed the rate and called `update()`, which read that stored frequency
against the new one. Past the new Nyquist the prewarping tangent is negative,
`g` comes out negative with it, and the section doubles its own output.

Measured across every one of the eleven response types and eight pairs of
rates: a non-finite output in 2001 samples going from 192 kHz to 48 kHz, 807
samples from 96 kHz to 8 kHz, 3370 at the slowest. Forty milliseconds at the
fast end. Not a drift, and nothing reports it: the driver change that caused
it was minutes earlier and the symptom is in the mixer.

Worth recording that the quiet cases are wrong too. 96 kHz down to 44.1 kHz
stays stable, because the stale corner lands on a part of the tangent that
happens to be positive, and what it gives is a 46 kHz corner read as about a
kilohertz. A stable filter at entirely the wrong frequency, which is the
version of this somebody would file as "the EQ sounds different today".

**Fixed** by clamping in `update()`, on the way to the coefficient, so the
ceiling is measured against whatever the rate is by then and `frequency`
keeps what was asked for. The floor is applied first and the ceiling last,
which also fixed the ordering fault sitting next to it, exactly as in
`OnePole`: as one expression, a sample rate below about two hertz left the
one hertz floor above its own ceiling.

**2. An unconfigured section gave two different wrong answers, and neither
was the filter its own members describe.** Every `a` started at zero, so
`process` returned silence for ever, while `magnitudeAt` saw a zero `a2`,
took its degenerate branch and reported unity at every frequency. The members
said a one kilohertz lowpass at 44.1 kHz.

The pairing is the worst possible one. An equaliser drawing its curve before
anybody had touched a band would have drawn a flat line through unity, which
is exactly what a bypassed band should look like, while the same band in the
signal path deleted everything through it. Correct on screen and silent in
the audio.

**Fixed** by building the coefficients in a constructor. The useful
consequence is the same one that night found: `a1` and `a2` are now non-zero
at every reachable setting, which makes the degenerate branch a backstop
rather than a state the studio can be in. `setSampleRate` also now requires
a finite rate as well as a positive one, because infinity passes a bare
`> 0.0` and is the one value that makes the prewarping tangent exactly zero.

### Reachability: neither one, today, and the third is an accident

Both are latent. Said plainly because the honest answer matters more than a
dramatic one.

- **`LowPassGate`** calls `filter.set` every sample inside `process`, so it
  cannot hold a stale corner.
- **`PassFilter::setSampleRate`** passes the rate down and then calls its own
  `update()`, which re-sets every section from the frequency it was asked
  for. It keeps the raw frequency, so this is correct.
- **`Voice`** escapes by an accident of ordering in a different file, and
  this is the one worth writing down. It guards `filter.set` on the modulated
  cutoff or Q having actually moved, which is a performance guard
  `CLAUDE.md` describes and defends. `Voice::setSampleRate` does not forget
  that guard. So after a rate change the stale coefficients would stay for
  the life of a held note, and the only reason they do not is that
  `PanOne::prepareToPlay` happens to follow `setSampleRate` with `setPatch`,
  which ends in `retune()`, which forgets it. Nothing anywhere says that
  `Voice::setSampleRate` leaves the filter needing repair, and the clamp
  does bind there in ordinary use: the voice's filter runs at four times the
  rate and its cutoff is `patch.cutoff * 2^octaves`, so a 20 kHz cutoff with
  the default half keytracking on a high note asks for 138 kHz, which the
  ceiling catches at 48 kHz base.
- **`BellMatch`** and **`PanOneParams`** hold no `Svf` at all. The second
  only converts the type enum, and `toEnum (v, 10)` against a ten entry
  choice list is correct and deliberate: the matched bell is an equaliser
  band and is not offered as a voice filter.

### What the old test was missing, and why

It was a good test and the gap was structural rather than careless.

**Every check set the sample rate first and the frequency second.** That is
the order a test naturally writes, and the order the file's own `make()`
helper hard codes. Nothing in 1083 lines ever moved the rate afterwards, so
fault 1 was not merely unchecked, it was unreachable by the shape of the
harness.

**Nothing ever looked at a filter that had not been handed a rate and a
setting first**, so fault 2 was invisible for the same kind of reason.

**`isMatched` was never called, and `bellMatched` appeared once** in the
whole file, inside the curve check. The one response with a second design
behind it was also the one whose neutral setting nothing checked.

**The curve check stopped at 0.45 of the rate.** The top twentieth of the
band was never compared against the real response, which is precisely where
a bell's skirt is crushed and where the matched design exists to do something
about it.

**Every clamp in the file could be removed without a single check
noticing**, and two of them are load bearing rather than decorative. See the
mutation results.

### What is now checked

`Tests/SvfTest.cpp`, 1083 lines to 1338, with the pole modulus factored out
and shared rather than written twice.

- **A filter told the rate has changed is indistinguishable from one built at
  that rate in the first place.** Bit for bit, over 2772 settings: all eleven
  responses, 36 pairs of rates from 8 to 192 kHz, and seven fractions of the
  first rate's band including past its own ceiling. No reference arithmetic
  and no setting at which it may be approximately true. The pole modulus is
  read off the state transition matrix beside it, because an equality between
  two filters would hold just as well if both were unstable, and the output
  is run for 250000 samples at the rate pairs that used to run away, which is
  more than twenty times the slowest old failure.
- **A corner above the ceiling is the corner at the ceiling, exactly**, at
  four rates and five Q values. This is the one that catches the clamp being
  applied to the coefficients but not to the matched bell's design, which has
  a seam in it: `bellIsMatchable`'s boundary rises with Q and crosses the
  ceiling around a Q of ninety, so above that a band asked for a cutoff past
  the ceiling is feasible at the ceiling and infeasible at the frequency
  asked for, and designed from the wrong one it drops silently to the
  conventional bell. Only visible at a high Q.
- **An unconfigured filter is the lowpass its members describe**, in both
  answers. The agreement is asserted against the running filter rather than
  between the two formulas, because silence from `process` and unity from
  `magnitudeAt` are each perfectly self consistent. A step settles at unity,
  it is three decibels down at a kilohertz, it passes nothing at Nyquist and
  it falls like two poles.
- **`isMatched` tells the truth**: never true where `BellMatch`'s own
  feasibility predicate says there is no design, always true where there is
  one and a gain to correct, false for every other type even after a matched
  bell was set on the same object, and where it falls back the coefficients
  are the plain bell's to the last bit. 1868 settings matched and 607 fell
  back.
- **A matched bell of no gain is a wire**, bit for bit on 2000 random samples
  and unity on the drawn curve. `neutralAtZeroGain` never reached this type.
- **Nonsense in the rate, the corner, the Q and the gain still leaves a
  filter that runs**, over 11550 settings, and the invariant the header rests
  its stability argument on is asserted directly: `a1`, `a2` and `a3` all
  strictly positive, which is `g` strictly positive and finite said in terms
  of what the filter holds. That is a far sharper instrument than waiting for
  an output to stop being finite, because a corner that has come out a hair
  negative gives a pole modulus of 1.0000000000000002, which runs away in
  seconds and passes any tolerance anybody would write around one. Finite
  nonsense plus infinity, no NaN: the line is where a value is still well
  defined, and neither this header nor `OnePole.h` claims to repair a
  not-a-number from outside.
- **The curve now runs to 0.4998 of the rate** rather than 0.45.

### Mutation testing

Fifty seven deliberate breakages of `Source/Fx/Svf.h`, each compiled with
`-fsanitize=address,undefined -fno-sanitize-recover=undefined` exactly as
`Tests/run.sh` has them. Against the test as it was found, 45 were caught.
Against the test as it now stands, **52 were caught**. All five survivors are
genuine no-ops rather than gaps, and all five are now written down in the
header.

The seven the old test missed, all of which now fail:

- the two faults above, put back (`set` clamping against the rate of the
  moment; the constructor removed);
- the floor and the ceiling on the corner, each removed;
- the floor and the ceiling applied in the wrong order;
- the lower bound on Q removed;
- the bound on the gain removed;
- the matched bell designed from the unclamped frequency;
- `setSampleRate` accepting a non-finite rate.

Two of those clamps turned out to be load bearing rather than decorative,
which is worth knowing because they read as defensive boilerplate:

- **The one hertz floor on the corner.** A cutoff of nothing gives a `g` of
  nothing and arrives at exactly the unconfigured filter's pair of
  contradictory answers. Reachable, and not obscurely: the instrument
  modulates its cutoff in octaves, twelve of them downward, so a patch
  resting at the bottom of its range sweeps to 20 hertz over 4096, which is
  a fifth of a hundredth of a hertz.
- **The bound on the gain.** Past about 170 decibels of cut the amplitude
  underflows to zero, and a shelf divides its `g` by the square root of it,
  so `g` becomes infinite, `a1` becomes zero and `a2` is infinity times zero.
  The filter then outputs silence from the first sample, permanently.

The five survivors, all genuine no-ops:

- **The degenerate guard at the top of `magnitudeAt` removed.** Unreachable
  now that the constructor exists and the clamps hold, which the strictly
  positive check turns from an argument into a reading over 11550 settings.
- **`g` recovered as `a2 / a1` rather than `a3 / a2`.** Both are `g`.
- **The curve's own ceiling taken at Nyquist rather than 0.4999 of the
  rate.** Every response tends to `m0` as the prewarped frequency runs away
  and both ceilings land on that asymptote to within absolute rounding, so
  the answer does not move by anything a tolerance can read. Measured as 508
  dB apart in a ratio, which is two ways of writing a number that is
  effectively zero: the types where it differs are the ones whose asymptote
  is zero. It stays because the denominator carries the square of the
  tangent, which overflows once the corner's own tangent falls below about
  1e-138, needing a sample rate of that order.
- **The first clamp on the frequency `magnitudeAt` is asked about.** The
  `fmin` below it already holds the top end, and a magnitude is even in the
  prewarped frequency, so folding a negative one to DC and reflecting it give
  the same number. Dead either way.
- **The upper bound on Q.** A musical bound, and the header says so: a
  positive `g` and a positive `k` are stable at any Q.

### Checked and found sound

- **Every mixing weight, one response at a time.** Twenty two breakages
  across the ten plain responses, the bell and both shelves, including the
  whole highpass polarity flipped, which leaves every magnitude in the file
  identical. All caught, most of them by the structural identities rather
  than by the magnitude checks.
- **The recursion and the coefficient algebra.** Thirteen breakages: both
  integrators turned to Euler, the state updates reordered so one reads the
  other, every sign in `v1`, `v2` and `v3`, the output weights swapped, and
  `a1`, `a2`, `a3` and `k` each built wrong. All caught.
- **The drawn curve against the running filter**, which caught five of six
  breakages of `magnitudeAt`'s algebra.
- **`PassFilter` and `LowPassGate`'s handling of a rate change**, by reading
  them, as above.
- **`PanOneParams`' filter type conversion** against its choice list.

### Left alone deliberately

- **A not-a-number handed in from outside.** Neither this file nor
  `OnePole.h` guards it, and a test asserting it did would be asserting a
  guard nobody has decided to write. If it is wanted it belongs in both
  files on the same night.
- **`PassFilter`'s own default state.** A `PassFilter` that is processed
  without `set` or `setSampleRate` ever being called used to output silence,
  because its first section was an unconfigured `Svf`. Tonight's constructor
  fixes that as a side effect: it is now the 20 hertz second order highpass
  its members describe. Nothing in `Source` constructs one yet, so there is
  no fault to report, and its own test is somebody else's night.
- **The curve's behaviour above Nyquist.** The drawn curve holds the top of
  its own range while the state space form walks `z` round the circle and
  comes back to DC. Both are honest answers to a question nobody should ask,
  and nothing asserts which happens.

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
