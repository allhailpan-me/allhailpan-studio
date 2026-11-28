#!/usr/bin/env python3
"""
Mutation testing for the send tap point: Source/SendTap.h and the pre fader
figure in Source/SoloGraph.h, against Tests/SendTapTest.cpp and
Tests/SoloGraphTest.cpp.

Each mutation is one deliberate, individually plausible mistake: a sign, an
index, a constant, a clause dropped, two arms of a ternary swapped. The kind of
thing somebody could write believing it, not a random character swap.

Every mutation is run against BOTH tests rather than only the one that owns the
header, because the two overlap on purpose: SoloGraphTest's forward pass reads
SendTap::levelTarget and SendTap::sourceIsPreFader to model what arrives at the
master, so a mistake in the table can be caught by the measurement as well as
by the table's own test. The report says which test noticed, and a mutation
that only one of them sees is worth knowing about.

Why it matters here more than usual. Nothing in this feature can announce that
it is wrong. Both taps hold the right strip at a plausible level, so an
inverted choice is a reverb following a fader it was told not to follow: no
click, nothing on a meter, no error. The audit that preceded this file found
the tap's own tooltip describing the opposite of the shipped default, and it had
been there four days.

The broken copies are compiled WITH THE SANITIZERS, with exactly the flags
Tests/run.sh uses. That is not a detail. CLAUDE.md records that four deliberate
breakages of bounds checks elsewhere in this project looked like they survived,
and every one of them was an out of bounds read that the sanitized suite catches
and a plain -O2 build does not. A mutation harness weaker than the real test run
reports gaps that are not there and hides the ones that are.

    python3 Research/send-tap-mutations.py            all of them
    python3 Research/send-tap-mutations.py 3 11        just those two
"""

import concurrent.futures
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FLAGS = [
    "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter",
    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
    "-fno-sanitize-recover=undefined", "-D_GLIBCXX_ASSERTIONS", "-pthread",
]

# Every test that reads any of this. Both are run against every mutation.
TESTS = ["SendTapTest.cpp", "SoloGraphTest.cpp"]

# (short name, what it breaks, header, text to find, text to put there)
MUTATIONS = [
    # ---- the numbers an .ahp carries -------------------------------------
    ('stored numbers swapped', 'every saved project sends from the other point',
     'SendTap.h',
     '            case Tap::postFader:  return 0;\n            case Tap::preFader:   return 1;',
     '            case Tap::postFader:  return 1;\n            case Tap::preFader:   return 0;'),

    ('stored read inverted', 'a project saved post fader loads as pre fader',
     'SendTap.h',
     '        return stored == 1 ? Tap::preFader : defaultTap;',
     '        return stored == 0 ? Tap::preFader : defaultTap;'),

    ('unknown stored reads pre fader', 'a tap this version cannot model plays from a point it cannot model',
     'SendTap.h',
     '        return stored == 1 ? Tap::preFader : defaultTap;',
     '        return stored != 0 ? Tap::preFader : defaultTap;'),

    ('the default moves', 'every project saved before the choice existed changes behaviour',
     'SendTap.h',
     '    constexpr Tap defaultTap = Tap::postFader;',
     '    constexpr Tap defaultTap = Tap::preFader;'),

    # ---- which buffer, which is the quiet one ----------------------------
    ('source buffer inverted', 'a reverb follows a fader it was told not to follow',
     'SendTap.h',
     '    inline bool sourceIsPreFader (Tap tap) noexcept  { return tap == Tap::preFader; }',
     '    inline bool sourceIsPreFader (Tap tap) noexcept  { return tap == Tap::postFader; }'),

    ('carriesFader inverted', 'the label and the behaviour disagree about the fader',
     'SendTap.h',
     '    inline bool carriesFader (Tap tap) noexcept  { return tap == Tap::postFader; }',
     '    inline bool carriesFader (Tap tap) noexcept  { return tap == Tap::preFader; }'),

    ('carriesPan inverted', 'the label and the behaviour disagree about the pan',
     'SendTap.h',
     '    inline bool carriesPan   (Tap tap) noexcept  { return tap == Tap::postFader; }',
     '    inline bool carriesPan   (Tap tap) noexcept  { return tap == Tap::preFader; }'),

    ('names swapped', 'the menu says the tap the send is not taken from',
     'SendTap.h',
     '        return tap == Tap::preFader ? "pre fader (post FX)"\n                                    : "post fader (post pan)";',
     '        return tap == Tap::preFader ? "post fader (post pan)"\n                                    : "pre fader (post FX)";'),

    ('short name swapped', 'the button says one tap and the tooltip the other',
     'SendTap.h',
     '        return tap == Tap::preFader ? "pre" : "post";',
     '        return tap == Tap::preFader ? "post" : "pre";'),

    # ---- the cut, which is the only number the taps differ in ------------
    ('cut tap inverted', 'a post fader send stops carrying the cut and a pre fader one always does',
     'SendTap.h',
     '        return tap == Tap::postFader || muteAffectsPreFaderSends;',
     '        return tap == Tap::preFader || muteAffectsPreFaderSends;'),

    ('cut needs the setting', 'a post fader send carries the cut only while the setting is on',
     'SendTap.h',
     '        return tap == Tap::postFader || muteAffectsPreFaderSends;',
     '        return tap == Tap::postFader && muteAffectsPreFaderSends;'),

    ('cut ignores the setting', 'the setting on a strip menu does nothing at all',
     'SendTap.h',
     '        return tap == Tap::postFader || muteAffectsPreFaderSends;',
     '        return true;'),

    ('post fader folds the cut in too', 'a wet path arrives at the square of the cut and collapses',
     'SendTap.h',
     '        if (tap != Tap::preFader)\n            return sendLevel;',
     '        if (tap != Tap::preFader)\n            return sendLevel * preFaderCutGain;'),

    ('pre fader drops the cut', 'a silenced strip feeds its reverb at full level',
     'SendTap.h',
     '        return sendLevel * preFaderCutGain;',
     '        return sendLevel;'),

    ('an unreadable cut reads as unity', 'a mute that does nothing',
     'SendTap.h',
     '        if (! (preFaderCutGain >= 0.0 && preFaderCutGain <= 1.0))\n            return 0.0;',
     '        if (! (preFaderCutGain >= 0.0 && preFaderCutGain <= 1.0))\n            return sendLevel;'),

    ('an unreadable level is clamped', 'a level a file should not hold arrives at the nearest end',
     'SendTap.h',
     '        if (! (sendLevel >= 0.0 && sendLevel <= 1.0))\n            return 0.0;',
     '        if (! (sendLevel >= 0.0 && sendLevel <= 1.0))\n            return sendLevel > 1.0 ? 1.0 : 0.0;'),

    # ---- the crossfade between the two taps ------------------------------
    ('fade in the travelled form', 'the far endpoint is out by a rounding error, which is a click at each join',
     'SendTap.h',
     '        return from * (1.0 - t) + to * t;',
     '        return from + (to - from) * t;'),

    ('fade weights swapped', 'the crossfade runs backwards',
     'SendTap.h',
     '        return from * (1.0 - t) + to * t;',
     '        return from * t + to * (1.0 - t);'),

    ('fade driven by progress not by what is left', 'the five milliseconds run the wrong way round',
     'SendTap.h',
     '        return fadeSample (fromSample, fromGain, toSample, toGain, 1.0 - remaining);',
     '        return fadeSample (fromSample, fromGain, toSample, toGain, remaining);'),

    ('fade applies one gain to a blend', 'the measured burst on a muted strip comes back',
     'SendTap.h',
     '        const double from = fromSample * fromGain;\n        const double to   = toSample   * toGain;',
     '        const double from = fromSample * toGain;\n        const double to   = toSample   * toGain;'),

    # ---- the pre fader copy guard ----------------------------------------
    ('copy guard drops the anticipation', 'the block a send leaves the pre fader tap reads a buffer nothing filled',
     'SendTap.h',
     '        return sourceIsPreFader (tap)\n            || sourceIsPreFader (activeTap)\n            || (fade > 0.0 && sourceIsPreFader (fadeFrom));',
     '        return sourceIsPreFader (tap)\n            || (fade > 0.0 && sourceIsPreFader (fadeFrom));'),

    ('copy guard drops the running fade', 'the fade off the pre fader tap loses what it is fading from',
     'SendTap.h',
     '        return sourceIsPreFader (tap)\n            || sourceIsPreFader (activeTap)\n            || (fade > 0.0 && sourceIsPreFader (fadeFrom));',
     '        return sourceIsPreFader (tap)\n            || sourceIsPreFader (activeTap);'),

    ('routing guard drops the running fade', 'routeSends declines the copy its own fade is about to read',
     'SendTap.h',
     '        return sourceIsPreFader (tap)\n            || (fade > 0.0 && sourceIsPreFader (fadeFrom));',
     '        return sourceIsPreFader (tap);'),

    ('routing guard never refuses', 'every insert pays for a copy nothing reads',
     'SendTap.h',
     '        return sourceIsPreFader (tap)\n            || (fade > 0.0 && sourceIsPreFader (fadeFrom));',
     '        return sourceIsPreFader (tap)\n            || (fade >= 0.0 && sourceIsPreFader (fadeFrom));'),

    # ---- the figure SoloGraph hands the pre fader point ------------------
    ('pre fader figure inverted', 'the setting does the opposite of what the menu says',
     'SoloGraph.h',
     '        return SendTap::carriesStripCut (SendTap::Tap::preFader,\n                                         settings.muteAffectsPreFaderSends)\n                 ? mainGain : 1.0f;',
     '        return SendTap::carriesStripCut (SendTap::Tap::preFader,\n                                         settings.muteAffectsPreFaderSends)\n                 ? 1.0f : mainGain;'),

    ('pre fader figure always cuts', 'the cue path cannot be had at all',
     'SoloGraph.h',
     '        return SendTap::carriesStripCut (SendTap::Tap::preFader,\n                                         settings.muteAffectsPreFaderSends)\n                 ? mainGain : 1.0f;',
     '        return mainGain;'),

    ('pre fader figure never cuts', 'a silenced strip is always heard through its pre fader send',
     'SoloGraph.h',
     '        return SendTap::carriesStripCut (SendTap::Tap::preFader,\n                                         settings.muteAffectsPreFaderSends)\n                 ? mainGain : 1.0f;',
     '        return 1.0f;'),

    ('pre fader figure asks about the wrong tap', 'the figure for one point answered for the other',
     'SoloGraph.h',
     '        return SendTap::carriesStripCut (SendTap::Tap::preFader,\n                                         settings.muteAffectsPreFaderSends)\n                 ? mainGain : 1.0f;',
     '        return SendTap::carriesStripCut (SendTap::Tap::postFader,\n                                         settings.muteAffectsPreFaderSends)\n                 ? mainGain : 1.0f;'),
]

# A survivor recorded here is a judgement that the mutation changes no property
# the code exists to produce. Anything not in here that survives is a gap in
# the test and the harness exits non-zero.
EXPECTED_SURVIVORS = {
}


def run_one(index):
    name, breaks, header, old, new = MUTATIONS[index]

    with tempfile.TemporaryDirectory() as work:
        # THE WHOLE OF Source IS COPIED AND THE MUTATION APPLIED IN THE COPY,
        # rather than putting one mutated header on an include path ahead of
        # the real tree. That is not belt and braces, it is the only thing that
        # works, and the first version of this file got it wrong in a way that
        # reported a clean result.
        #
        # A quoted include resolves against the directory of the file doing the
        # including before it looks at any -I path. Source/SoloGraph.h includes
        # "SendTap.h", so with a mutated SendTap.h on the include path and the
        # real Source behind it, SoloGraphTest.cpp compiled against the REAL
        # header and could not see the mutation at all. Every SendTap.h
        # mutation was being run against one test while the report claimed two.
        #
        # CLAUDE.md records the same mechanism as the reason a quoted include
        # inside a subdirectory of Source has to be written relative to its own
        # file. It is worth knowing that it bites a mutation harness too.
        source = os.path.join(work, "Source")
        shutil.copytree(os.path.join(ROOT, "Source"), source)

        path = os.path.join(source, header)
        text = open(path).read()

        if text.count(old) != 1:
            return (index, name, breaks, "ANCHOR",
                    "the text to mutate appears %d times in %s" % (text.count(old), header))

        open(path, "w").write(text.replace(old, new, 1))

        env = dict(os.environ)
        env["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=0"
        env["UBSAN_OPTIONS"] = "print_stacktrace=1"

        noticed = []

        for test in TESTS:
            binary = os.path.join(work, test.replace(".cpp", ""))

            compiled = subprocess.run(
                [os.environ.get("CXX", "g++")] + FLAGS
                + ["-I", os.path.join(ROOT, "Tests", "juce-stub"),
                   "-I", source,
                   "-o", binary, os.path.join(ROOT, "Tests", test)],
                capture_output=True, text=True)

            if compiled.returncode != 0:
                # A mutation that will not compile could never ship, but it is
                # recorded separately because the test is not what noticed.
                return (index, name, breaks, "NO BUILD",
                        (compiled.stderr.strip().splitlines() or [""])[-1])

            try:
                ran = subprocess.run([binary], capture_output=True, text=True,
                                     timeout=900, env=env)
            except subprocess.TimeoutExpired:
                noticed.append(test.replace("Test.cpp", "") + " did not finish")
                continue

            if ran.returncode != 0:
                lines = [l for l in ran.stdout.splitlines() if "FAIL" in l]
                detail = lines[0].strip() if lines else "exited %d" % ran.returncode
                noticed.append("%s: %s" % (test.replace("Test.cpp", ""), detail))

        if noticed:
            return (index, name, breaks, "CAUGHT", " | ".join(noticed))

        return (index, name, breaks, "SURVIVED", "")


def main():
    wanted = [int(a) for a in sys.argv[1:]] or list(range(len(MUTATIONS)))

    results = []

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        for result in pool.map(run_one, wanted):
            results.append(result)
            index, name, breaks, verdict, detail = result
            print("%-10s %2d  %-44s  %s" % (verdict, index, name, detail))
            sys.stdout.flush()

    caught = sum(1 for r in results if r[3] == "CAUGHT")
    nobuild = sum(1 for r in results if r[3] == "NO BUILD")
    survived = [r for r in results if r[3] == "SURVIVED"]
    anchor = [r for r in results if r[3] == "ANCHOR"]

    print()
    print("%d mutations: %d caught by the tests, %d would not compile, %d survived, "
          "%d anchors stale" % (len(results), caught, nobuild, len(survived), len(anchor)))

    unexpected = [r for r in survived if r[1] not in EXPECTED_SURVIVORS]

    for r in survived:
        if r[1] in EXPECTED_SURVIVORS:
            print("  expected survivor    %2d  %s" % (r[0], r[1]))
            print("                           because %s" % EXPECTED_SURVIVORS[r[1]])
        else:
            print("  UNEXPECTED SURVIVOR  %2d  %s (%s)" % (r[0], r[1], r[2]))

    # The other direction: an expected survivor that is now caught means the
    # reason written beside it has stopped being true, or a test has grown to
    # cover it, and either way this file is out of date.
    ran = {MUTATIONS[r[0]][0] for r in results}
    caught_names = {r[1] for r in results if r[3] == "CAUGHT"}

    now_caught = sorted(n for n in EXPECTED_SURVIVORS if n in ran and n in caught_names)

    for name in now_caught:
        print("  NO LONGER A NO-OP    %s" % name)
        print("                       it was expected to survive because %s"
              % EXPECTED_SURVIVORS[name])

    for r in anchor:
        print("  STALE ANCHOR         %2d  %s  %s" % (r[0], r[1], r[4]))

    if not unexpected and not anchor and not now_caught:
        print()
        print("Every mutation was caught, or survived for a reason this file records.")

    return 0 if not unexpected and not anchor and not now_caught else 1


if __name__ == "__main__":
    sys.exit(main())
