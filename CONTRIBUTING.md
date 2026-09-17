# Contributing to ALLHAILPAN Studio

This is a DAW for the people. Musicians and developers are both welcome, and
you don't need to write C++ to be useful here.

## Ways to help

- **Use it and report what breaks.** Open an issue with what you did, what you
  expected, and what happened. Your audio interface, sample rate, buffer size
  and the plugins involved all help. Screenshots and short screen recordings
  are ideal.
- **Tell us what feels wrong.** Workflow friction is a real bug. If something
  takes four clicks that should take one, say so.
- **Test plugins.** Report any VST3 that fails to load, makes no sound, or
  behaves oddly, along with its version.
- **Write code.** See below.

## Building

You need CMake 3.22+, a C++20 compiler, and an internet connection for the
first build (JUCE and Rubber Band are downloaded automatically).

- **Windows:** open the folder in Visual Studio 2022 (with the "Desktop
  development with C++" workload) and press Ctrl+F5.
- **macOS:** `cmake -B build -G Xcode && cmake --build build`
- **Linux:** install the JUCE dependencies, then
  `cmake -B build && cmake --build build -j`

## Pull requests

- Keep each pull request to one change, and say what it does and how you tested it.
- Match the surrounding style: JUCE spacing, `camelCase` members, clear names
  over clever ones, comments that explain *why*.
- **Never allocate, lock, or log on the audio thread.** The audio callback may
  only use lock-free structures, and `juce::SpinLock::ScopedTryLockType` where
  the message thread swaps state.
- Test with a real audio device and at least one VST3 before submitting.
- By contributing, you agree your work is licensed under AGPLv3.

## Licence and name

The code is AGPLv3. The ALLHAILPAN name and logos are not covered by it: see
[TRADEMARKS.md](TRADEMARKS.md). Forks are welcome, under a different name and
without the logos.

## Code of conduct

Be decent. Assume good faith, keep criticism about the work, and leave the
gatekeeping at the door. Beginners belong here.
