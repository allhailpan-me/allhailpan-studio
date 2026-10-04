# ALLHAILPAN Studio

A free, open-source digital audio workstation with an FL Studio style workflow: channel rack, step sequencer, piano roll, pattern-based playlist, mixer with inserts and sends, audio recording, and VST3 plugin hosting.

Built for the people. Free forever, source included.

> Early development, but the engine is real. Plugin delay compensation, tempo-locked modulators on any plugin parameter, broadcast-standard loudness metering, and per-output routing for multi-output drum machines all work today.

## What makes it different

- **Modulators on anything.** A tempo-locked shape wired to any parameter of any plugin you host, assigned by grabbing the knob rather than hunting through a list. Several can stack on one parameter, and they ride on top of whatever the knob is already set to.
- **A mix report, not a magic knob.** The studio measures the finished master to ITU-R BS.1770-4, the same standard streaming services normalise to, and tells you plainly what the numbers mean. Verified against EBU Tech 3341 compliance test cases to within 0.011 LU. A plugin only hears its own insert; the studio hears the whole thing.
- **Delay compensation that understands sends.** Look-ahead plugins hold audio back, and a bus fed by another insert cannot be ready before its source. Every path is levelled so instruments, audio clips and sends reach the master on the same sample.
- **Each drum on its own strip.** Multi-output instruments like Microtonic Multi can send every output bus to its own mixer insert, with its own fader and effects.

## Runs on

- **Windows** 10 and 11 (WASAPI, DirectSound, optional ASIO)
- **macOS** (CoreAudio, VST3 and Audio Units)
- **Linux** (ALSA and JACK, VST3 and LV2)

Every push is built automatically for all three systems. Download the latest builds from the **Actions** tab.

## Build it yourself

### Windows

1. Install **Visual Studio Community** (2022 or newer) with the **Desktop development with C++** workload. This includes CMake.
2. Install **Git** from https://git-scm.com.
3. Open **Developer PowerShell for Visual Studio** and run:

```powershell
git clone https://github.com/allhailpan-me/allhailpan-studio.git
cd allhailpan-studio
cmake -B build
cmake --build build --config Release
```

The first configure downloads JUCE, which takes a few minutes. The app appears at:

```
build\AllHailPanStudio_artefacts\Release\ALLHAILPAN Studio.exe
```

You can also open the folder directly in Visual Studio (File > Open > Folder); it recognises CMake projects.

#### Optional: ASIO

Download the ASIO SDK from Steinberg's developer site, unzip it, and configure with:

```powershell
cmake -B build -DAHP_ASIO_SDK_DIR="C:/path/to/asiosdk"
```

### macOS

Install Xcode from the App Store, then CMake (`brew install cmake`), and run:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

### Linux (Debian or Ubuntu)

```bash
sudo apt install build-essential cmake git libasound2-dev libjack-jackd2-dev ladspa-sdk \
  libfreetype-dev libfontconfig1-dev libx11-dev libxcomposite-dev libxcursor-dev \
  libxext-dev libxinerama-dev libxrandr-dev libxrender-dev libglu1-mesa-dev mesa-common-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

## Using it

**Views:** Playlist (**F5**), Channel rack (**F6**), Piano roll (**F7**), Modulators (**F8**), Mixer (**F9**), and the mix report, switched with the tabs at the top.

### Transport
- **Space** plays and pauses. **Stop** returns to the start marker. Pressing **Stop** again when already stopped does three things: silences every note, resets the instruments (which also stops plugins that drone on regardless of note-offs), and sends the start marker back to the beginning, so the next **Play** starts from bar 1. Clicking the ruler or an empty spot afterwards sets a new start point as usual.
- **Click the ruler** to play from that point. Click empty playlist space to move the playhead there; double-click it to play from there.
- Playback **loops at the end of the last clip**, not the end of the grid. The faint line in the playlist marks that point.

### Projects
- **File** menu (top left): New (**Ctrl+N**), Open (**Ctrl+O**), Open recent, Save (**Ctrl+S**), Save as (**Ctrl+Shift+S**).
- A project (`.ahp`) stores:
  - The whole arrangement: clips, MIDI notes, automation, stretch and pitch.
  - Track names, mutes and routing, plus tempo.
  - The full mixer, with every instrument and effect **including its settings** (your Massive patch, your LANDR settings, and so on).
- Projects go in `Documents/ALLHAILPAN Studio/Projects` by default. The window title shows the project name, with `*` when there are unsaved changes. You're asked to save before closing, opening or starting a new project.
- **Samples:** a project remembers where each audio file is (full path, plus its location relative to the project).
  - **Save with samples** copies every file into a `<project> Samples` folder beside the project, so you can move the folder or share it.
  - If files are missing when opening, those clips show a red header. Use **File > Find missing samples** to pick a folder, and they're matched by name.
- **Missing plugins** (for example, a project opened on another computer) are listed when opening. Their saved settings stay in the project, so saving again doesn't lose them.
- **Autosave** runs every two minutes while you have unsaved changes. If the app closes unexpectedly, you're offered your work back the next time it starts.

### Exporting
- **File > Export song to WAV** renders the whole arrangement to a 24-bit WAV at your device's sample rate. It runs offline, so it's much faster than playing the song through.
- **File > Export stems** writes one file per mixer insert, plus the full mix, into a folder. Each stem is post-fader and post-effects, so they add back up to the mix.
- Exports include everything: instruments, audio clips, automation, mixer levels and all effects, with four seconds of tail so reverbs ring out.
- The app goes quiet while rendering and a progress window lets you cancel. Your playback position is left where it was.

### Undo and redo
- **Ctrl+Z** undoes, **Ctrl+Y** (or **Ctrl+Shift+Z**) redoes. The **Undo** and **Redo** buttons are at the top right.
- There is no step limit. Every finished edit is one step: moving, trimming, stretching, pasting, deleting, recording, note and automation edits, clip settings, track names, mutes and routing.
- Loading plugins and moving mixer faders are not undo steps yet.

### Instruments (channel bar)
- 16 instrument channels. Pick one in **Channel**, then load an instrument into it. **Show** reopens its window, **Unload** removes it. **Mixer** chooses which insert it plays through.
- Live playing goes to the selected channel: a MIDI controller, the on-screen piano, or the computer keyboard (**Z to /** lower octave, **Q to P** upper, **Up/Down** change octave).

### Instruments that play themselves
Drum machines and sequencer plugins (Microtonic, arpeggiators, loop players) don't need you to play notes. They follow the app's transport, so press **Play** and they run at the project tempo. The app reports the full position to them: tempo, bar and beat, time, loop points, and whether it's playing or recording.

**Multi-output plugins:** some instruments put each sound on its own output bus instead of one stereo mix. **Microtonic Multi** does this: its A/B selector is switched off and each of the 8 drum channels goes to a separate stereo output, so a host reading only the first output hears one drum, or nothing.

When a plugin has more than one output bus, ALLHAILPAN Studio switches them all on and mixes them into the channel. The **Mix all outs** button in the channel bar turns that off if you'd rather hear only the first stereo output.

To give each drum its own mixer strip, use **Route outs...** in the channel bar and choose **Spread across inserts from here**. Every output lands on its own insert in order, each with its own fader, effects and metering, the way a hardware drum machine's individual outs would be patched. Delay compensation follows the split, so a plugin with latency on one drum's strip does not pull that drum out of time with the rest of the kit.

### Recording
- Choose the mode next to **Rec**:
  - **Auto** records MIDI if the selected channel has an instrument, otherwise audio from your interface.
  - **Input audio**, **MIDI + automation**, **Input audio + MIDI**.
  - **Instrument sound** (and **Instrument sound + MIDI**) records the channel's own output as audio. Use it for plugins that make sound the app can't capture as notes.
- **Notes played inside a plugin's window** are recorded too, but only if the plugin offers a MIDI output. The app checks when you load it and says so if it doesn't, and the status bar reports how many notes came from the plugin after each take. Synplant does: clicking and dragging its branches plays notes and transmits them, so they land in the piano roll and stay editable. Anything the plugin passes straight through from your keyboard isn't recorded twice.
- **Count-in:** the box next to the mode counts you in with the metronome for 1 or 2 bars before recording starts. The song stays silent during the count, the clock counts down in red, and recording begins exactly on the start marker. It's remembered between sessions.
- Press **Rec** (**Ctrl+R**). It records onto the armed track, or the first empty one.
- MIDI recording captures the notes you play **and every knob you grab in the plugin's window**. Only knobs you actually move are recorded: plugins that animate their own controls (Synplant, for one) would otherwise fill the clip with noise. Some plugins don't report knob grabs at all, and the status bar says so after recording. The result is a MIDI clip; double-click it to open the piano roll.
- Audio takes are saved as 24-bit WAV in `Documents/ALLHAILPAN Studio/Recordings`, lined up for your interface's latency.

### Input monitoring
- The **Monitor** box next to the record mode passes your input through the studio so you hear it with your own effects: an amp simulator on a guitar, reverb on a vocal. **armed** only passes it through while a track is armed, **on** always does.
- It is heard through a mixer insert, so put the effects you want to hear on that insert. By default it is insert 1.
- **Use headphones.** Monitoring through speakers with a microphone open feeds back, and an amp simulator makes that loud. The status bar shows **MONITORING** whenever input is passing through.
- The monitor path deliberately skips delay compensation. That compensation lines internal paths up with each other, and on a monitor path it would only be latency you feel while playing. Lining the take up with the arrangement is handled separately, when the take is recorded.
- Turning it on and off fades over about five milliseconds rather than switching, so it cannot click.
- Your interface's own direct monitoring is still the lowest latency route, and is worth using if your buffer size is large.

### Playlist
- Drop audio from the browser or Explorer/Finder. Drag clips to move them. **Shift+drag** duplicates.
- **Edges:** with **Stretch** off, dragging an edge trims (cuts off) the clip. With **Stretch** on, it time-stretches instead. **Shift** does the opposite of the button.
- **Selecting:** click a clip; **Ctrl+click** adds or removes one; **Ctrl+drag** draws a selection box (add **Shift** to keep the current selection); **Ctrl+A** selects all.
- **Editing:** **Ctrl+C** copy, **Ctrl+X** cut, **Ctrl+V** paste at the mouse (or at the playhead), **Ctrl+B** duplicate, **Delete** removes. Right-click also deletes.
- **Snap** runs from Bar down to 1/6 step or None; hold **Alt** to ignore it. **Ctrl+scroll** zooms.
- **Clip bar** (bottom), for one selected clip:
  - Audio clips: **Pitch** (plus or minus 24 semitones), **Stretch**, **Source BPM**, **Sync to tempo**, **Gain**, **Reset**.
  - **Sync to tempo** matches audio recorded elsewhere to your project. Tell the clip the tempo it was recorded at and it stays on the grid at any project tempo, and follows if you change the tempo later. Time-stretching does not touch pitch, so a vocal stays in key. Leave the tempo blank and it works one out from the clip's length.
  - MIDI clips: which channel the clip plays, a button to open it, and **Clear auto** to delete recorded knob moves.
- Stretching and pitch use Rubber Band's highest-quality engine. It renders in the background, and a quick preview plays until the render is done.
- Right-click a track name to send its audio to a mixer insert, arm it, or mute it.

### Channel rack

- **F6.** One row per instrument channel, one column per step. Click to draw, drag across to paint, right-click a row for fills on the beat, every other step, or every fourth.
- Steps are not a separate kind of data: each one is an ordinary note in an ordinary MIDI clip, so a pattern drawn here opens in the piano roll and can be edited there.
- Each channel remembers which **key** its steps play, which matters for drum plugins that put every sound on a different note. Changing that key moves the row's notes rather than appearing to wipe them.
- Pattern length (1, 2 or 4 bars) and resolution (8, 16 or 32 steps to the bar) are per project, with **<** and **>** to move to another bar.
- How full a block is drawn shows its velocity. **Shift+drag** up or down on a lit step sets that velocity directly, which is how hardware step sequencers do accents. Without varied velocity a programmed beat sounds like a machine.

### Groove
- **Swing** in the channel rack sets where the offbeat sits inside its pair. 50 is straight, 66 lands it on the third triplet, which is a full shuffle. That is the definition hardware samplers established, so a swing number means the same thing here as it does to a drummer or another studio. Most records sit between 54 and 62.
- **Feel** goes further than swing, which on its own still sounds programmed. A played pattern has offbeats that are slightly quieter and slightly early or late, so **played** and **loose** soften and drift them by increasing amounts.
- The drift is derived from each note's own position rather than drawn fresh, so a pattern plays the same way twice and an export matches what you heard. A groove that wandered on every pass would be unusable.
- Groove never changes the stored notes. It is applied as the arrangement is handed to the engine, so it can be dialled while playing and turned off without having lost the original timing, and notes you played off the grid are left where you played them.

### Modulators

- **F8.** A modulator is a shape that runs in time, wired to any number of plugin parameters. It is the difference between drawing a filter sweep by hand every eight bars and saying "this moves".
- Because modulators drive ordinary parameters, they work on **any plugin you host**, not only on built-in devices.
- **Add a modulator, click Learn, then grab the knob you want** inside the plugin's own window. No hunting through a list of several hundred numbered parameters.
- Seven shapes: sine, triangle, saw, ramp, square, random, and sample and hold. Rates are musical divisions from 1/16 to 8 bars, so everything stays locked to the tempo.
- **Bipolar** swings either side of the knob's own value; **unipolar** only moves it upward. Depth is per target, and can be negative to invert.
- Modulation rides on top of whatever the parameter is already set to, so the knob still means what it means. Turn it by hand and the modulator moves around the new value. Several modulators on one parameter add up.
- While the transport rolls they follow the song, so a project sounds the same every time and matches its export. While stopped they free-run, so you can dial a sound in.
- The random shapes are repeatable: the same beat gives the same value on every play.

### Mix report

- Measures the master **after every effect on it**, which is what actually leaves the studio.
- **Loudness** in LUFS to ITU-R BS.1770-4, the standard Spotify, Apple Music and YouTube normalise to. Pick your target and it tells you how far off you are and what that costs.
- **True peak**, **loudness range** (how much dynamic life is left), and **mono compatibility** (whether parts of the mix will cancel on a club system).
- Each reading comes with what it means in plain language: that being louder than streaming wants buys nothing because they turn you down, that a true peak above -1 dB can distort after MP3 encoding even though it measures clean now, that a very small loudness range is what a limiter doing too much looks like.
- Verified against EBU Tech 3341 compliance test cases 1 and 2 at 44.1, 48 and 96 kHz, within 0.011 LU of the required reading against a tolerance of 0.1.

### Piano roll
- Click to draw notes, drag to move, drag the right edge to resize, right-click (or **Delete** tool) to erase. Click the keys to hear notes.
- The bottom lane shows **Velocity**, or any **automation** you recorded. Click to add points, drag them, right-click to delete. **Clear lane** removes a lane.

### Mixer
- The **Mixer** button (or **F9**) opens the mixer in its own window, so you can keep the playlist behind it. It remembers its size and position.
- **Master on the left**, then the 16 inserts in a grid that wraps to more rows instead of stretching across the screen.
- Each strip has a fader (double-click for 0 dB), pan, mute, a stereo meter, its level in dB, and how many effects it holds. Double-click a name to rename it; right-click for reset and clear options.
- Click a strip to show its **effect rack** on the right: 8 slots, each with an on/off button for bypass.
- Clicking an empty slot opens a **search window**: type a few letters, use Up/Down, then Enter to load. Works the same for replacing an effect.
- **Find...** in the channel bar searches your instruments the same way.
- **Sends:** each insert has two, below the effect rack. Pick a destination and a level to feed part of this insert into another one, so a reverb or delay can be shared instead of loaded on every track. A send can only feed a later insert, which is what keeps the mixer free of feedback loops, so the destination list offers only those.
- For mastering, select **Master** and add LANDR there. The metronome and browser previews skip the master rack, so your chain never processes the click.

### Other
- **Plugins** opens the plugin manager (*Options > Scan for new or updated VST3 plug-ins*).
- The status bar shows your device, latency, CPU load, and input and output levels.

### Not yet
- Warp markers, for audio that drifts within a single take. Clip-level tempo matching handles audio recorded at a steady tempo.
- No instruments are bundled yet, so you need your own VST3s to make sound.

## Roadmap

Done:

- Audio engine, transport, metronome, device settings, plugin scanning
- Playlist with audio clips, snapping, trim, copy and paste, marquee select
- Piano roll, and automation lanes inside MIDI clips
- VST3 instruments in 16 channels, effects in mixer inserts
- Mixer with inserts, FX slots, metering and sends
- Audio and MIDI recording, including parameter automation and notes played inside a plugin's own window
- Time-stretching and pitch-shifting with Rubber Band, and per-clip tempo matching
- Project save and load including plugin state, autosave, crash recovery, WAV and stem export
- Channel rack and step sequencer
- Plugin delay compensation, and denormal protection
- Modulators on any plugin parameter
- Loudness metering and the mix report
- Per-output bus routing for multi-output plugins
- Out-of-process plugin scanning, so a plugin that crashes cannot take the studio with it

Next:

- Input monitoring, and loop recording
- Warp markers
- A bundle of open-source instruments, so a fresh install makes sound on its own
- Sampler channels, and a built-in synth
- Standalone automation clips
- MIDI file import and export

## Project layout

```
Source/        C++ application code
Assets/        Logos baked into the app
prototype/     Browser prototype, the design reference for the native app
.github/       Automatic builds for Windows, macOS and Linux
```

## License

ALLHAILPAN Studio is licensed under the **GNU Affero General Public License v3.0**. See `LICENSE`.

Third-party components:

- [JUCE](https://juce.com), AGPLv3
- [VST3 SDK](https://github.com/steinbergmedia/vst3sdk) by Steinberg Media Technologies, MIT. VST is a registered trademark of Steinberg Media Technologies GmbH.
- [Rubber Band Library](https://breakfastquay.com/rubberband/) by Particular Programs Ltd, GPL v2 or later (downloaded at build time)

The ALLHAILPAN name and logo are not covered by the code license. See `TRADEMARKS.md`.

## Making a release build

The Debug build is for development. For a program you can keep or share, build Release:

1. In Visual Studio, change the configuration dropdown in the toolbar from **x64-Debug** to **x64-Release**.
2. Wait for CMake to finish, then **Build > Build All** (Ctrl+Shift+B).
3. The app appears at `out\build\x64-Release\AllHailPanStudio_artefacts\Release\ALLHAILPAN Studio.exe`.

That .exe is self-contained: the C++ runtime is linked in, so it runs on a machine with no Visual Studio installed. Copy it anywhere, pin it to the taskbar, and it will find your audio device and plugin list through the settings it stores in your user folder.

If you share the .exe, AGPLv3 requires the matching source to be available to whoever receives it. Publishing it from this repository's Releases page satisfies that.

## Contributing

Bug reports, workflow complaints, plugin testing and code are all welcome. See [CONTRIBUTING.md](CONTRIBUTING.md).
