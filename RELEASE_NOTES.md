Everything since v0.1.2, which was ten features ago. The engine changed more in this release than the interface did.

## Since v0.2.0

**Warp markers.** Tempo matching fixes audio played at one steady tempo. A warp marker fixes audio that drifts inside the take, which is the normal case for a live performance or a vocal. Pin a point in the audio to a position on the grid and everything between two markers is stretched to fit: put a marker on the snare that landed late, drag it onto the beat, and the bar either side comes with it. Select an audio clip and double-click it to add one. A warped clip always follows the project tempo, and the waveform is drawn through the warp, so a transient appears under the grid line you pinned it to.

**Input monitoring.** Hear the interface input through the studio's own effects while you play. Off, armed only, or always, with a level control and a choice of insert. Deliberately not delay compensated: that compensation lines internal paths up with each other, and on a monitor path it would only be latency you feel directly. Lining the take up with the arrangement is still the recorder's job.

**Groove, and per-step velocity in the channel rack.** A feel applied to playback rather than written into the notes, so it can be changed or turned off without having lost the original timing.

## The big ones

**Plugin delay compensation.** Plugins that look ahead (mastering processors, look-ahead limiters, spectral effects) hold audio back. Nothing accounted for that, so a track carrying one played late against the others, by an amount that changed every time a plugin was loaded. Every path is now levelled, including sends and playlist audio, so everything reaches the master on the same sample. The status bar reports the compensation when it is not zero.

**Modulators.** A tempo-locked shape wired to any parameter of any plugin you host. Add one, click Learn, then grab the knob you want inside the plugin's own window. Seven shapes, rates from 1/16 to 8 bars, bipolar or unipolar, any number of targets each with its own depth. Modulation rides on top of whatever the knob is set to, so turning it by hand still works and the modulator moves around the new value.

**Mix report.** Measures your master after every effect on it, to ITU-R BS.1770-4, the standard streaming services normalise to. Integrated loudness, true peak, loudness range and mono compatibility, each with what it actually means. Verified against EBU Tech 3341 compliance test cases at 44.1, 48 and 96 kHz, within 0.011 LU against a tolerance of 0.1.

**Channel rack.** One row per channel, one column per step, on F6. Steps are ordinary notes in ordinary MIDI clips, so a pattern drawn here opens in the piano roll and can be edited there.

**Mixer sends.** Two per insert, post-fader, so a reverb can be shared instead of loaded on every track.

**Per-output routing.** Multi-output instruments like Microtonic Multi can send each output bus to its own mixer insert, every drum with its own fader and effects. Route outs... in the channel bar.

**Tempo matching.** Tell an audio clip the tempo it was recorded at and it stays on the grid at any project tempo, and follows if you change the tempo later. Pitch is unaffected, so a vocal stays in key.

## Also

- Plugins are scanned in a separate process, so one that crashes on load can no longer take the studio down with it
- Denormal protection, which is the CPU spiking seconds after sound stops
- JUCE pinned to 9.0.3 rather than tracking master, so this builds the same way in a year

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

- **No ASIO in these builds.** Steinberg's licence does not allow redistributing the SDK, so the downloadable Windows build uses Windows Audio only. For ASIO, download the SDK yourself and build with `-DAHP_ASIO_SDK_DIR=` pointing at it.
- **No instruments are bundled**, so you need your own VST3s to make sound.
- **macOS and Linux builds are lightly tested.** Please report anything broken.

Licensed under AGPL-3.0. The name and logo are excluded, see TRADEMARKS.md.
