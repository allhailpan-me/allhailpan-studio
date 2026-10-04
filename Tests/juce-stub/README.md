# Empty JUCE module headers

Every file in here is empty on purpose.

A header in `Source/` that is worth testing often needs only a few things from
JUCE: `jmax`, an `AudioBuffer`, a `String`. It still has to `#include` the
whole module to get them, and that module cannot be compiled in a few seconds,
which would be the end of testing the header at all.

So this directory sits on the include path ahead of JUCE and satisfies that
include with nothing. The test file then declares, before including the header
under test, exactly the `juce::` types that header uses, and nothing else. If
the header later reaches for something the stub does not define, the test stops
compiling and says which type it was, which is a far better outcome than a
missing piece of behaviour going unnoticed.

Those hand written stubs tend to be stricter than the real thing. The
`AudioBuffer` in `LatencyDelayTest.cpp` bounds checks its accessors, where
JUCE's does not in a release build, so an index JUCE would quietly accept is
caught. That is the point: the stub is a place to be paranoid.

This is not a JUCE reimplementation and must not grow into one. Anything that
needs real JUCE behaviour belongs in the application, where CI compiles it
against the real thing.
