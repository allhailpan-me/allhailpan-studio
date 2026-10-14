# Hardening log

What each night audited, how hard it tried to break it, what it found, and
what it judged to be a genuine no-op rather than a gap. A target recorded as
clean here does not need re-auditing unless the code has changed since.
Newest first.

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
