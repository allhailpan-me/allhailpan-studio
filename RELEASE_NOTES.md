Monitoring you can actually play through, a front panel on the instrument, and the settings have somewhere to live. Below that, the instrument itself, two fixes found by playing v0.3.0, and then everything that went into v0.3.0 itself.

## Since v0.5.0

**Monitoring, properly this time.** v0.5.0 moved the studio onto a faster
driver, and that was necessary but not sufficient: it was still not playable,
and the report that it was not came with the detail that settled it, which was
that the same machine is fine in another studio. That rules out the hardware.
Four things were wrong, and two of them were ours.

*The buffer was four times larger than it needed to be.* Microsoft document
the standard Windows audio driver as supporting buffer sizes from 128 samples
up to 480, with 480 being what every application gets by default. The studio
was aiming at five milliseconds, which asks for 240 samples and therefore
takes 256, stepping straight over 128 and 192. Most of the gain from the
faster driver was being thrown away immediately. It now lands on 128.

*The buffer size control really does nothing on plain Windows Audio.* This was
reported and assumed to be a misunderstanding. It is not: in that mode JUCE
publishes a single buffer size and ignores any request to change it, because
the wakeup period belongs to the driver. If you were changing that setting and
hearing no difference, you were right.

*You could hear yourself twice.* A send is a copy of a channel taken after its
fader, so on the channel you are monitoring through it carries your own
playing. The direct path was exempted from delay compensation and the send
copy was not, so your input arrived once immediately and again at send level,
separated by the longest plugin lookahead in the project and moving every time
a plugin was loaded anywhere. Anybody who has put a reverb on a vocal while
tracking it was hearing that.

*Anything routed straight to the master played early.* The opposite problem. A
synth or a playlist clip sent directly to the master waited for far less than
the rest of the mix, so it sat ahead of everything by whatever lookahead
happened to be loaded on any insert. Nothing in the original report pointed at
this; writing the test found it.

Both of those last two survived because they were somewhere no test could
reach, and this is the arithmetic in the studio least able to announce that it
is wrong. Nothing errors, nothing crackles: tracks simply sit a few
milliseconds apart and the record sounds slightly smeared. Both had a comment
above them claiming the opposite of what the code did.

So the whole compensation graph is now plain arithmetic in a file of its own,
and the test does not check the formulas. It walks every route through a
generated mixer, totals everything on the path, and asserts that all of them
reach the master on the same sample: twenty thousand mixers, with sends chained
forward, output buses split across destinations and lookahead plugins anywhere.
Monitoring is asserted as the deliberate exception, early by exactly the amount
skipped and with its send copy not put back. Then the file was broken on
purpose twenty seven ways, including both original bugs reintroduced verbatim,
and all twenty seven were caught.

**A "Find the fastest device" button,** in Preferences > Audio. The automatic
search runs only on a first run, because a settings file that already names a
device is a choice the studio has to assume you made. That left everybody
upgrading without the fix, which is most of the people who need it. The button
asks for the same search on purpose and prints what it found underneath.

**Exclusive mode is offered rather than taken.** It is the fastest thing
Windows has, and it holds your sound card: nothing else on the computer makes
a sound while the studio is open. Somebody who turned monitoring on and found
their browser silent would have no way to connect the two. So the studio names
it where the latency is shown, with what it costs, and leaves the choice alone.
For what it is worth, FL Studio's own driver is documented as multi-client, so
sharing the device is the same choice it makes.

## Since v0.4.0

**Monitoring is low enough to play through without you setting anything.** This is the second half of a fix. The first half, in v0.4.0, was a real defect: the monitored signal was being held back by plugin delay compensation. What was left was not a defect in the studio's code at all, which is why it survived that fix: the studio was accepting whatever driver Windows offered, and what Windows offers by default is tuned for playing a video back without glitching rather than for hearing yourself in time. That is tens of milliseconds, and no buffer size setting recovers it.

Windows has been offering a faster route to the same device the whole time. Alongside the shared mode everything gets by default, it exposes the same device through IAudioClient3, which JUCE calls Windows Audio (Low Latency Mode): single digit milliseconds, and still shared, so a video in a browser keeps playing. The studio was not asking for it.

So on first run, and only on first run, the engine measures what monitoring costs and, if it is too slow to play through, works down a list until something is fast enough. The choice is then saved as if you had made it, so this happens once, and a machine that was already fast enough is not touched at all.

Exclusive Mode is deliberately not on that list, although it would win. It holds the sound card, so nothing else on the machine plays while the studio is open, and somebody who asked to hear their guitar and found their browser had gone silent would have no way to connect the two: they would report a second fault rather than the price of the first one being fixed. The studio names it where the latency is shown, and leaves it to you.

**If you are upgrading, press the button.** The search only runs when there is no saved audio device, because a settings file that already names one is a choice the studio has to assume you made. **Preferences > Audio** has a *Find the fastest device* button that asks for the same search on purpose, with what it found printed underneath.

Buffer size is chosen the same way, against a five millisecond target rather than the smallest the driver will admit to. A buffer at the floor is where dropouts live, and a studio that crackles until you raise a setting is a worse first impression than one four milliseconds slower than it could be. If you want the floor, it is still yours to take.

Turning monitoring on now also prints the round trip in milliseconds, and says plainly when it is too slow to play through rather than leaving you to work that out with an instrument in your hands.

The ordering and the arithmetic are in a header with no JUCE in it, so they are tested rather than asserted: the buffer choice is checked against an independently written second implementation over twenty thousand generated driver lists, with sizes drawn at the boundaries because that is where it can be wrong. Nineteen deliberate breakages of that header, including putting the buffered shared mode back at the top of the preference list, are all caught by the tests.

**A preferences window,** on Ctrl+, or from the File menu. The audio device was behind one button and the plugin list behind another, with no relationship between the two windows, and three more settings were constants nobody could reach. All of it is in one window now, in three tabs:

- **General:** how often the autosave runs, including off; how loud the metronome is; and the tempo a new project starts at. Each applies as you change it, so you can set the click level while the metronome is running rather than guessing and reopening the window. There is a button to open the folder your settings, plugin list and autosave live in, and one to put those three back to their defaults.
- **Audio:** the device chooser, with the monitoring round trip still printed in milliseconds underneath it.
- **Plugins:** scanning, exactly as before.

The **Audio** and **Plugins** buttons in the toolbar now open that window on those tabs, because picking a device and scanning for plugins are what a fresh install needs and neither should be behind a tab. Monitoring, the record mode and the count-in stay on the toolbar: they are decisions you make between takes, not settings you go looking for.

The metronome level was a fixed number in the engine before this, and the default is that same number to within a tenth of a decibel, so your click does not change level on upgrade. It is read once per click rather than per sample, so turning it down while the metronome is running cannot step on a click that is already decaying. The autosave default is the same two minutes it has always been.

**PAN One has a front panel.** The instrument shipped in v0.3.1 with no controls at all, so its window opened black: there were no parameters, and the generic editor every host falls back on had nothing to draw. There are eighteen controls now, grouped into oscillators, filter, amplitude envelope and filter envelope, with the presets in a box at the top. A marker appears when the knobs no longer match the preset they came from, so a preset that has stopped sounding like itself explains itself.

They are ordinary plugin parameters, which means **the modulators reach into PAN One** exactly as they reach into anything else you host: wire a shape to its cutoff and it sweeps. The note in v0.3.1 saying that needed real work first was simply wrong, and reading the framework's own source rather than trusting that note is what settled it.

Projects now save the knobs rather than only the preset number, written by name so that a later version adding a control in the middle cannot shift your saved values along by one. A project saved before the knobs existed carries a preset number and nothing else, and loads as that preset.

The knobs and the presets are a table in a header with no JUCE in it, rather than four hand written lists in four places, which is what lets a test check the things that would otherwise be silent: that no two controls write the same part of the patch, that every value in every preset is inside the range of the control that will hold it, and that the panel draws every control exactly once. Twenty one deliberate breakages of that table are all caught.

**A monitoring fault that a settings file could cause.** The monitoring mode, the record mode and the count-in were restored from the settings file without checking that the stored value was one the control offers. An unknown value leaves a combo box with nothing selected, which reads back as zero, and for monitoring that zero became a mode the engine does not have: the callback took its low latency path, which deliberately skips delay compensation, while the mixer matched neither "armed" nor "on" and passed no input through. The result would have been monitoring that is silent and costs the compensation anyway, which is a hard thing to diagnose from the symptom. No released version wrote a value that could trigger it, so nobody will have seen this, but a hand edited or future written settings file would have been enough.

Related, and the reason the above was found: a setting that is not a number parses as zero, and zero is a real value for several of these. It is off for the autosave and full level for the metronome, so a single garbled line in the settings file would have read back as a deliberate choice, and for the click as the loudest one in the range. Settings are now checked before they are acted on, and anything unreadable falls back to its default.

## Since v0.3.1

**It comes with an instrument.** Until now the studio was silent on a new machine until you supplied your own VST3s, which is where most people who try a studio stop trying it. PAN One is built in: two oscillators with selectable shapes and detune, a sub an octave down, a resonant filter with its own envelope, an amplitude envelope, sixteen voices, and five presets (Sub Bass, Soft Keys, Slow Pad, Short Pluck, Reed Lead).

It is in the instrument list the first time you open the studio. There is nothing to scan for and nothing to download, and it behaves as an ordinary plugin everywhere else: it loads the same way a VST3 does, saves into a project the same way, and records, modulates and mixes through the same code rather than through a special case.

The oscillators are band limited. A naive saw or square is a discontinuity sampled directly, and everything above Nyquist folds back as partials that are not harmonically related to the note, so they move the wrong way as you play up the keyboard. That shimmer in the top two octaves is what gives a soft synth away, and no filter afterwards removes it. Measured against the naive version at 2 kHz, the saw puts 17 dB less energy into those partials and the square 18 dB less.

The filter is a state variable design with the integrators kept as integrators, which matters because a filter envelope changes the cutoff every single sample. The usual alternative clicks on fast sweeps and can be driven unstable by the modulation alone. Its resonant peak lands on the cutoff to four decimal places from a Q of 0.5 to 10.

## Since v0.3.0

**Monitoring is no longer delayed by plugin delay compensation.** Playing a guitar through the studio was pushed back by an amount that had nothing to do with the audio interface, and that changed whenever a plugin was loaded or bypassed. Monitored input is mixed into an insert, and that insert was then held back along with everything else to keep the tracks lined up with each other. A lookahead limiter or a linear phase EQ anywhere in the project was enough on its own to make the instrument unplayable.

The insert being monitored through is no longer held back. Anything else landing on that insert plays early by the same amount while monitoring is on, which is the right trade while tracking: the player has to be able to play, and lining the take up with the arrangement afterwards is the recorder's job and already happens.

**The audio settings window now says what your latency actually is.** A buffer size in samples does not tell you what you have chosen until it is divided by the sample rate and added to the driver's own buffering either side. The round trip is now printed in milliseconds, split into input, output and buffer, taken from what the driver reports rather than inferred.

If the number is high on Windows, the buffer size is often not the lever. Shared mode Windows Audio and DirectSound buffer heavily whatever is asked of them. Changing **Audio device type** to Windows Audio (Exclusive Mode) is usually a large improvement. ASIO is better still, but note that these downloads are built without it: `JUCE_ASIO` only compiles in when the Steinberg SDK is present, which the build machine does not have. Building locally with `AHP_ASIO_SDK_DIR` set enables it.

**Plugin editors and the mixer stay in front.** Opening the mixer over the playlist and clicking an insert appeared to do nothing: the editor opened behind the main window with no sign it had opened at all. Editors now come to the front when opened, and both they and the mixer float above the main window while the studio is the application in front. They drop back when you switch to something else, so they will not sit over your browser, and while a dialog is open so nothing covers it.

## Since v0.2.0

**Loop recording, take folders and comping.** Set a loop range by dragging in the ruler and record over it, and each time round is kept as its own take rather than recorded over the pass before. The passes arrive as one take folder on the track: one clip holding several alternatives, each still written to its own WAV in `Recordings`. Recording the same loop again adds to the folder already there.

A take folder draws one lane per pass, with the part that is being heard lit and the parts you have not chosen dimmed but still present. Drag across a lane to give that stretch of the song to that pass, so the first line can come from pass three and the next from pass seven. Double-click a lane to give it the whole folder, Alt-click to throw a pass away. The strip along the bottom shows the comp itself.

Every join is crossfaded, ten milliseconds by default, with an equal-power curve. Cutting at a zero crossing is not enough between two different performances: the waveform is continuous at the join but its slope is not, and a voice clicks on that. Equal power is the right shape because two takes of one singer are not the same waveform, so their powers add rather than their amplitudes, and a linear fade dips audibly at every join. The comp is held in seconds into the folder rather than in beats, so changing the tempo moves the folder on the grid without moving the joins through the performance.

The takes are never touched, so a choice can be remade at any time, including after the project has been closed and reopened, and the export reads the comp the same way playback does rather than through a second code path. A comp swipe is one undo step.

**Automation on the playlist, and on the mixer.** Automation could only live inside a MIDI clip, which left two things impossible to move at all: anything on a mixer insert, and anything that needs to run across a section rather than within one pattern. Both work now. Right-click a playlist track header, choose Automate, and pick an insert's volume or pan, a parameter of any effect on it, or a parameter of any instrument you have loaded. What appears is a clip: drag it, copy it, trim it, mute it and delete it like audio or MIDI, drawn as the curve rather than as a block. It starts at the value the control is already sitting at, so drawing one on a mix you have balanced does not throw the balance away, and the mixer's own fader follows the curve as it plays.

Every segment has a bend, dragged from the circle between two points, because a straight line is the wrong shape for most of what automation is asked to do: a filter that should sit closed for four bars and then open over one is two points with a bend, not a dozen points that are then impossible to move as a gesture. The bend warps the position along the segment rather than the value, which is what guarantees it can never take the value outside the two points it joins, so what you hear is the line on the screen rather than a spline bulging past it. Two points on one beat is an instant jump.

**MIDI file import and export.** A part written here can now be taken somewhere else, and a part written anywhere else can be brought in. Drop a `.mid` file on the playlist and you get one clip per track in the file, placed where you dropped it and routed to free instrument channels, with the playlist tracks named after the tracks in the file; a single track carrying several MIDI channels is split by channel rather than flattened into one unplayable pile. The file's own tempo is adopted while the project is still at the tempo nobody chose, and left alone once you have set one. Export writes the arrangement, or just the selected clips, as a standard multi-track file with the tempo and the track names. The round trip is exact for notes: a file exported and imported again gives back the same starts, lengths, pitches and velocities. Automation is not exported, because a MIDI file has no way to say which plugin parameter a lane belongs to, and the export says so rather than pretending.

**Warp markers.** Tempo matching fixes audio played at one steady tempo. A warp marker fixes audio that drifts inside the take, which is the normal case for a live performance or a vocal. Pin a point in the audio to a position on the grid and everything between two markers is stretched to fit: put a marker on the snare that landed late, drag it onto the beat, and the bar either side comes with it. Select an audio clip and double-click it to add one. A warped clip always follows the project tempo, and the waveform is drawn through the warp, so a transient appears under the grid line you pinned it to. Trimming and splitting understand markers; cutting a warped clip in two is inaudible.

**True peak, measured the way the standard defines it.** The peak meter now oversamples four times through the interpolator ITU-R BS.1770-4 Annex 2 tabulates, because the peak of the waveform your converter reconstructs is not the largest sample in the file. The meter here used to report the largest sample, which reads around three decibels low on exactly the material where it matters: a limited master that measures clean and then distorts after encoding. If a mix you measured before this release looked safe at -0.3 dB, measure it again.

**Loudness range now follows EBU Tech 3342.** It was the plain spread between the 95th and the 10th percentile of the short term readings, with no gating, so a quiet intro or a fade out became the bottom of the distribution and a track that never changes level could report tens of LU of dynamic range. It is now gated at -70 LUFS and then at 20 LU below the mean of what survives that, which is what the standard specifies and what other meters report.

**Groove.** Swing in the channel rack sets where the offbeat sits inside its pair: 50 is straight, 66 lands it on the third triplet, which is a full shuffle. That is the definition hardware samplers established, so the number means the same thing here as it does to a drummer or another studio. Feel goes further, because swing on its own still sounds programmed: a played pattern has offbeats that are slightly quieter and slightly early or late, and played and loose soften and drift them by increasing amounts. The drift comes from each note's own position rather than from a live random source, so a pattern plays the same way twice and an export matches what you heard.

Groove never changes the stored notes. It is applied as the arrangement is handed to the engine, so it can be dialled while playing and turned off without having lost the original timing, and notes played off the grid are left where you played them.

**Per-step velocity.** How full a step is drawn in the channel rack shows its velocity, and Shift+drag up or down on a lit step sets it directly, which is how hardware step sequencers do accents.

**Input monitoring.** Hear the interface input through the studio's own effects while you play. Off, armed only, or always, with a level control and a choice of insert. Deliberately not delay compensated: that compensation lines internal paths up with each other, and on a monitor path it would only be latency you feel directly. Lining the take up with the arrangement is still the recorder's job.

**A test suite, run on every push.** JUCE takes minutes to compile, but most of what would be silently wrong in a studio is arithmetic that does not need it: where a warped clip reads from, how a comp crossfades, what a swing percentage means, what a loudness figure is. That arithmetic now lives in headers with no JUCE in them, with checks over it that run in about a minute with address and undefined behaviour sanitizers on, as their own CI job that reports before the platform builds have finished fetching their dependencies. A red test does not stop the build; it does stop a release from publishing. `./Tests/run.sh` runs the same thing locally.

## Fixed in this release

All of these were found by reviewing the work above rather than by anything going visibly wrong, which is the point: every one of them is the kind of fault that leaves the program running.

- **Picking a shuffle did nothing until you made another edit.** Groove is applied where the arrangement is handed to the engine, and the control set the value without telling anything, so the swing was stored, saved to the file and shown in the box while the pattern kept playing straight.
- **And it was not an undo step.** A groove change recorded no step, so the next Ctrl+Z undid the edit before it instead.
- **The loudness meter was allocating on the audio thread.** Reading the loudness range copied and sorted the entire short term history on every audio block, and the history grew for as long as the session stayed open: after an hour of playback that was a six-figure allocation and sort inside every five millisecond buffer, which is a dropout. The history is now kept as fixed histograms and reading a figure costs nothing.
- **Modulation allocated on the audio thread too**, on the first block after any plugin was loaded, and carried the previous plugin's resting values onto the new one when the two exposed the same number of parameters.
- **Monitoring then switching to an interface with no inputs** read past the end of the input array.
- **A plugin reporting a knob move from its own process call** could leave the audio thread spinning on a lock the message thread was holding across an allocation.
- **Changing the audio device during an export** could reallocate a buffer the export was writing through.
- **Every system exclusive message from a control surface** allocated on the audio thread.
- **File > New kept the old tempo**, so a MIDI file dropped into a fresh project decided the tempo had been chosen deliberately and arrived at the wrong speed without saying so.
- **A project file carrying both a comp and warp markers** loaded as a take folder with live markers on it, which nothing downstream expects.

## Before this release

**Plugin delay compensation.** Plugins that look ahead (mastering processors, look-ahead limiters, spectral effects) hold audio back. Nothing accounted for that, so a track carrying one played late against the others, by an amount that changed every time a plugin was loaded. Every path is now levelled, including sends and playlist audio, so everything reaches the master on the same sample. The status bar reports the compensation when it is not zero.

**Modulators.** A tempo-locked shape wired to any parameter of any plugin you host. Add one, click Learn, then grab the knob you want inside the plugin's own window. Seven shapes, rates from 1/16 to 8 bars, bipolar or unipolar, any number of targets each with its own depth. Modulation rides on top of whatever the knob is set to, so turning it by hand still works and the modulator moves around the new value.

**Mix report.** Measures your master after every effect on it, to ITU-R BS.1770-4, the standard streaming services normalise to. Integrated loudness, true peak, loudness range and mono compatibility, each with what it actually means. Verified against EBU Tech 3341 compliance test cases at 44.1, 48 and 96 kHz, within 0.011 LU against a tolerance of 0.1.

**Channel rack.** One row per channel, one column per step, on F6. Steps are ordinary notes in ordinary MIDI clips, so a pattern drawn here opens in the piano roll and can be edited there.

**Mixer sends.** Two per insert, post-fader, so a reverb can be shared instead of loaded on every track.

**Per-output routing.** Multi-output instruments like Microtonic Multi can send each output bus to its own mixer insert, every drum with its own fader and effects. Route outs... in the channel bar.

**Tempo matching.** Tell an audio clip the tempo it was recorded at and it stays on the grid at any project tempo, and follows if you change the tempo later. Pitch is unaffected, so a vocal stays in key.

**Also:** plugins are scanned in a separate process, so one that crashes on load can no longer take the studio down with it; denormal protection, which is the CPU spiking seconds after sound stops; and JUCE pinned to a release tag rather than tracking master, so this builds the same way in a year.

## Downloads

| Platform | File | Notes |
|---|---|---|
| Windows | `ALLHAILPAN-Studio-Windows.zip` | 64-bit, no installer needed |
| macOS | `ALLHAILPAN-Studio-macOS.zip` | Universal, Apple Silicon and Intel |
| Linux | `ALLHAILPAN-Studio-Linux.zip` | Needs ALSA or JACK |

All three are built by GitHub Actions directly from the source in this repository. The build log is under the Actions tab.

## Before you run it

Not code-signed, so every operating system will complain. Nothing is wrong with the file.

**Windows:** SmartScreen warns about an unknown publisher. More info, then Run anyway. Defender may also report `Trojan:Script/Wacatac.B!ml`, which is a false positive: the `!ml` suffix means a machine-learning guess rather than a signature match, and Wacatac is the bucket Defender uses for any unfamiliar unsigned executable. If it gets quarantined, Windows Security, Protection history, find the entry, Actions, Allow.

**macOS:** Gatekeeper refuses on a double-click. Right-click the app, Open, then Open again. If that fails, System Settings, Privacy and Security, scroll down, Open Anyway.

## Known limitations

- **No ASIO in these builds.** Steinberg's licence does not allow redistributing the SDK, so the downloadable Windows build uses the Windows Audio modes only. The studio picks the fastest of those it can on first run, which is usually enough to play through, but ASIO is lower still: download the SDK yourself and build with `-DAHP_ASIO_SDK_DIR=` pointing at it.
- **4/4 only, and one tempo for the whole song.** A MIDI file that changes tempo part way through is read at its opening tempo, and the status bar says so.
- **Loading a plugin and dragging a mixer fader are not undo steps.** Everything else that edits the arrangement is.
- **An instrument with 32 or more output channels** (sixteen stereo buses, which is the most this studio will route) makes the engine allocate once per block on the audio thread. Instruments with eight buses or fewer, which is all of the ones tested, are unaffected.
- **macOS and Linux builds are lightly tested.** Please report anything broken.
- **Nothing here has been through a long session on real hardware yet.** The arithmetic is tested; the feel is not. If something sounds wrong, that is worth a bug report even if you cannot say exactly what.

Licensed under AGPL-3.0. The name and logo are excluded, see TRADEMARKS.md.
