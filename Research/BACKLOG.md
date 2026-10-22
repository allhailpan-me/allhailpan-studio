# Research backlog

A running record of what each night looked at, what it built, what it rejected,
and why. Rejected ideas stay listed so a later night does not revisit them
blindly. Newest first.

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
arrives as distortion rather than hiss. Measured, a 1 kHz sine at 4 LSB has its
fifth and seventh harmonics at **-26 and -31 dB** below the fundamental
undithered, and at -74 dB or lower, which is the measurement floor, dithered.
It is also where the noise floor goes when the music gets quiet: the error
variance of an undithered 16 bit quantiser wanders over 3.6 dB as a sine fades,
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
safe rather than merely cheaper is that the export says in the status bar what
it is about to write, in words, before the file appears, so a wrong setting is
visible at the moment it matters rather than discoverable afterwards.

### What the test asserts

Properties, measured, against the published constants rather than against the
implementation's own formulas. `Tests/DitherTest.cpp`.

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
