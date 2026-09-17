# ALLHAILPAN Studio

A free, open-source digital audio workstation with an FL Studio–style workflow: channel rack, step sequencer, piano roll, pattern-based playlist, mixer with inserts, audio recording, and VST3 plugin hosting.

Built for the people. Free forever, source included.

> Early development. Working today: audio engine, VST3 instruments, MIDI and typing keyboard, sample browser, playlist with audio clips, and recording. The channel rack, piano roll and mixer are being ported from the browser prototype in `prototype/`.

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
git clone https://github.com/YOUR-NAME/allhailpan-studio.git
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

**Views:** Playlist (**F5**), Piano roll (**F7**), Mixer (**F9**), switched with the tabs at the top.

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

When a plugin has more than one output bus, ALLHAILPAN Studio switches them all on and mixes them into the channel. The **Mix all outs** button in the channel bar turns that off if you'd rather hear only the first stereo output. (If you want each drum on its own mixer insert, use the regular Microtonic on several channels for now; per-bus routing is on the roadmap.)

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

### Playlist
- Drop audio from the browser or Explorer/Finder. Drag clips to move them. **Shift+drag** duplicates.
- **Edges:** with **Stretch** off, dragging an edge trims (cuts off) the clip. With **Stretch** on, it time-stretches instead. **Shift** does the opposite of the button.
- **Selecting:** click a clip; **Ctrl+click** adds or removes one; **Ctrl+drag** draws a selection box (add **Shift** to keep the current selection); **Ctrl+A** selects all.
- **Editing:** **Ctrl+C** copy, **Ctrl+X** cut, **Ctrl+V** paste at the mouse (or at the playhead), **Ctrl+B** duplicate, **Delete** removes. Right-click also deletes.
- **Snap** runs from Bar down to 1/6 step or None; hold **Alt** to ignore it. **Ctrl+scroll** zooms.
- **Clip bar** (bottom), for one selected clip:
  - Audio clips: **Pitch** (±24 semitones), **Stretch**, **Gain**, **Reset**.
  - MIDI clips: which channel the clip plays, a button to open it, and **Clear auto** to delete recorded knob moves.
- Stretching and pitch use Rubber Band's highest-quality engine. It renders in the background, and a quick preview plays until the render is done.
- Right-click a track name to send its audio to a mixer insert, arm it, or mute it.

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
- For mastering, select **Master** and add LANDR there. The metronome and browser previews skip the master rack, so your chain never processes the click.

### Other
- **Plugins** opens the plugin manager (*Options > Scan for new or updated VST3 plug-ins*).
- The status bar shows your device, latency, CPU load, and input and output levels.

### Not yet
- Input monitoring through the app: use your interface's direct monitor.

## Roadmap

1. ~~Audio engine, transport, metronome, device settings, plugin scanning~~
2. ~~Playlist with audio clips, snapping, trim, copy/paste, marquee select~~
3. Channel rack and step sequencer, sampler channels, per-output-bus routing for multi-out plugins
4. ~~Piano roll~~ Built-in synth
5. ~~VST3 instruments in 16 channels, effects in mixer inserts~~
6. ~~Mixer with inserts, FX slots and metering~~ Sends
7. ~~Audio and MIDI recording, parameter automation recording~~ Input monitoring, loop recording
8. ~~Time-stretching and pitch-shifting with Rubber Band~~
9. ~~Project save and load (including plugin state), autosave~~ WAV and stem export
10. ~~Automation lanes in MIDI clips~~ Standalone automation clips

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
