#!/usr/bin/env python3
"""
Mutation testing for Source/ClipResampler.h against Tests/ClipResamplerTest.cpp.

Each mutation is one deliberate, individually plausible mistake: the kind of
thing somebody could write believing it, not a random character swap. The test
is required to notice every one of them. A test that passes deliberately broken
code is not a test.

The broken copies are compiled WITH THE SANITIZERS, with exactly the flags
Tests/run.sh uses. That is not a detail. CLAUDE.md records that four deliberate
breakages of bounds checks elsewhere in this project looked like they survived,
and every one of them was an out of bounds read that the sanitized suite catches
and a plain -O2 build does not. A mutation harness weaker than the real test run
reports gaps that are not there and hides the ones that are.

    python3 Research/clip-resample-mutations.py            all of them
    python3 Research/clip-resample-mutations.py 12 13      just those two
"""

import concurrent.futures
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "Source", "ClipResampler.h")
TEST = os.path.join(ROOT, "Tests", "ClipResamplerTest.cpp")

FLAGS = [
    "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter",
    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
    "-fno-sanitize-recover=undefined", "-D_GLIBCXX_ASSERTIONS", "-pthread",
]

# (short name, what it breaks, text to find, text to put there)
MUTATIONS = [
    ('bessel fixed ten terms', 'the window degrades silently as beta rises',
     '    for (int k = 1; k < 200; ++k)\n    {\n        term *= quarterSquared / ((double) k * (double) k);\n        sum  += term;\n\n        if (term < 1.0e-19 * sum)\n            break;\n    }',
     '    for (int k = 1; k < 10; ++k)\n    {\n        term *= quarterSquared / ((double) k * (double) k);\n        sum  += term;\n    }'),

    ('bessel loose stop', 'the series stops six digits early',
     '        if (term < 1.0e-19 * sum)',
     '        if (term < 1.0e-6 * sum)'),

    ('bessel k not k squared', 'the factorial in the denominator is k! not (k!)^2',
     '        term *= quarterSquared / ((double) k * (double) k);',
     '        term *= quarterSquared / (double) k;'),

    ('bessel half argument', 'the (z/2)^2 in the series becomes (z/2)',
     '    const double quarterSquared = z * z * 0.25;',
     '    const double quarterSquared = z * 0.25;'),

    ('beta wrong branch', 'the 21 to 50 dB branch used above 50 dB',
     '    if (attenuationDb > 50.0)\n        return 0.1102 * (attenuationDb - 8.7);',
     '    if (attenuationDb > 50.0)\n        return 0.5842 * std::pow (attenuationDb - 21.0, 0.4)\n             + 0.07886 * (attenuationDb - 21.0);'),

    ('beta 7.95 not 8.7', "the length formula's numerator used in the shape formula",
     '        return 0.1102 * (attenuationDb - 8.7);',
     '        return 0.1102 * (attenuationDb - 7.95);'),

    ('beta slope', '0.1102 becomes 0.1142',
     '        return 0.1102 * (attenuationDb - 8.7);',
     '        return 0.1142 * (attenuationDb - 8.7);'),

    ('length in radians', 'the 2.285 constant used with cycles per sample, 6.28 too short',
     '    return (attenuationDb - 7.95) / (14.36 * transitionCyclesPerSample);',
     '    return (attenuationDb - 7.95) / (2.285 * transitionCyclesPerSample);'),

    ('window no square root', 'the Kaiser argument without its square root',
     '        const double window   = besselI0 (beta * std::sqrt (inside < 0.0 ? 0.0 : inside))\n                                  / normaliser;',
     '        const double window   = besselI0 (beta * (inside < 0.0 ? 0.0 : inside))\n                                  / normaliser;'),

    ('window unnormalised', 'the window not divided by I0(beta)',
     '        const double window   = besselI0 (beta * std::sqrt (inside < 0.0 ? 0.0 : inside))\n                                  / normaliser;',
     '        const double window   = besselI0 (beta * std::sqrt (inside < 0.0 ? 0.0 : inside));'),

    ('window off by one', 'the half width taken as one entry too many',
     '        const double fraction = (double) j / halfWidthEntries;',
     '        const double fraction = (double) j / (halfWidthEntries + 1.0);'),

    ('window rectangular', 'no window at all, a truncated sinc',
     '        const double window   = besselI0 (beta * std::sqrt (inside < 0.0 ? 0.0 : inside))\n                                  / normaliser;',
     '        const double window   = 1.0;'),

    ('sinc evaluated at integers', 'sin(pi x)/(pi x) at a whole number of samples, which is 1.6e-14 and not zero',
     '            if (cutoffFraction == 1.0 && j % entriesPerCrossing == 0)',
     '            if (cutoffFraction == 1.0 && j == 0)'),

    ('sinc cutoff doubled', 'the cutoff an octave above the source Nyquist',
     '                const double x = 3.14159265358979323846 * cutoffFraction\n                               * (double) j / (double) entriesPerCrossing;',
     '                const double x = 2.0 * 3.14159265358979323846 * cutoffFraction\n                               * (double) j / (double) entriesPerCrossing;'),

    ('sinc cutoff halved', 'the cutoff an octave below the source Nyquist',
     '                const double x = 3.14159265358979323846 * cutoffFraction\n                               * (double) j / (double) entriesPerCrossing;',
     '                const double x = 0.5 * 3.14159265358979323846 * cutoffFraction\n                               * (double) j / (double) entriesPerCrossing;'),

    ('sinc centre zero', 'the centre tap zero instead of one',
     '                sinc = j == 0 ? 1.0 : 0.0;',
     '                sinc = 0.0;'),

    ('no per phase normalisation', 'every fractional position has its own slightly different gain',
     '        normalisePerPhase (built);',
     '        // normalisePerPhase (built);'),

    ('normalise by the wrong phase', 'the entry divided by the sum of a phase it is not in',
     '            const double sum = sums[(std::size_t) (j % entriesPerCrossing)];',
     '            const double sum = sums[(std::size_t) (j / entriesPerCrossing)];'),

    ('normalise by phase zero', 'every entry divided by the same sum',
     '            const double sum = sums[(std::size_t) (j % entriesPerCrossing)];',
     '            const double sum = sums[(std::size_t) 0];'),

    ('phase sum one wing', 'the phase sum taken over the right wing only',
     '            for (int i = -crossings + 1; i <= crossings; ++i)\n            {\n                const int entry = std::abs (i * entriesPerCrossing - phase);\n\n                if (entry >= 0 && entry < count)\n                    sum += built[(std::size_t) entry];\n            }',
     '            for (int i = 0; i <= crossings; ++i)\n            {\n                const int entry = std::abs (i * entriesPerCrossing - phase);\n\n                if (entry >= 0 && entry < count)\n                    sum += built[(std::size_t) entry];\n            }'),

    ('phase sum no reflection', 'the phase sum indexing without the absolute value',
     '            for (int i = -crossings + 1; i <= crossings; ++i)\n            {\n                const int entry = std::abs (i * entriesPerCrossing - phase);\n\n                if (entry >= 0 && entry < count)\n                    sum += built[(std::size_t) entry];\n            }',
     '            for (int i = -crossings + 1; i <= crossings; ++i)\n            {\n                const int entry = i * entriesPerCrossing - phase;\n\n                if (entry >= 0 && entry < count)\n                    sum += built[(std::size_t) entry];\n            }'),

    ('differences before normalising', 'the two tables describing different kernels',
     '        normalisePerPhase (built);\n\n        taps.resize ((std::size_t) count);\n        differences.resize ((std::size_t) count);\n\n        for (int j = 0; j < count; ++j)\n            taps[(std::size_t) j] = (float) built[(std::size_t) j];\n\n        // Built from the normalised values, which is the third of the three\n        // conditions in the header comment: differences straddle phase\n        // boundaries, so doing this first and normalising after would leave\n        // the two tables describing different kernels.\n        for (int j = 0; j + 1 < count; ++j)\n            differences[(std::size_t) j] = taps[(std::size_t) (j + 1)] - taps[(std::size_t) j];',
     '        taps.resize ((std::size_t) count);\n        differences.resize ((std::size_t) count);\n\n        for (int j = 0; j < count; ++j)\n            taps[(std::size_t) j] = (float) built[(std::size_t) j];\n\n        for (int j = 0; j + 1 < count; ++j)\n            differences[(std::size_t) j] = taps[(std::size_t) (j + 1)] - taps[(std::size_t) j];\n\n        normalisePerPhase (built);\n\n        for (int j = 0; j < count; ++j)\n            taps[(std::size_t) j] = (float) built[(std::size_t) j];'),

    ('differences reversed', 'h(j) - h(j+1) instead of h(j+1) - h(j)',
     '            differences[(std::size_t) j] = taps[(std::size_t) (j + 1)] - taps[(std::size_t) j];',
     '            differences[(std::size_t) j] = taps[(std::size_t) j] - taps[(std::size_t) (j + 1)];'),

    ('last difference not zeroed', 'the final entry extrapolated past the end of the table',
     '        differences[(std::size_t) (count - 1)] = 0.0f;',
     '        differences[(std::size_t) (count - 1)] = taps[(std::size_t) (count - 1)];'),

    ('no scaling at all', 'the cutoff never follows the lower Nyquist',
     '    if (! (step > 1.0))\n        return 1.0;\n\n    const double tracked = step > Kernel::maxTrackedStep ? Kernel::maxTrackedStep : step;\n    return 1.0 / tracked;',
     '    return 1.0;'),

    ('scale inverted', 'the kernel narrowed when it should widen',
     '    return 1.0 / tracked;',
     '    return tracked;'),

    ('scale below one too', 'the kernel scaled when reading slower than one to one as well',
     '    if (! (step > 1.0))\n        return 1.0;',
     '    if (! (step > 0.0))\n        return 1.0;'),

    ('no amplitude factor', 'the time scaling kept and the gain left at 1/step',
     '    out.left  = (float) (sumLeft  * scale);\n    out.right = (float) (sumRight * scale);',
     '    out.left  = (float) sumLeft;\n    out.right = (float) sumRight;'),

    ('no time factor', 'the amplitude scaled and the cutoff left at the source Nyquist',
     '    const double tableScale = scale * (double) perCrossing;',
     '    const double tableScale = (double) perCrossing;'),

    ('kernel truncated when scaled', 'the tap count not grown with the step, so the stretched kernel is cut off',
     '    const int half = (int) std::ceil ((double) kernel.crossings / scale);',
     '    const int half = kernel.crossings;'),

    ('half taps floored', 'the tap count rounded down rather than up',
     '    return (int) std::ceil ((double) kernel.crossings / scale);',
     '    return (int) ((double) kernel.crossings / scale);'),

    ('no reflection', 'the left wing indexed with a negative distance',
     '        const double distance = std::abs ((double) i - offset) * tableScale;',
     '        const double distance = ((double) i - offset) * tableScale;'),

    ('reflection by addition', 'the fraction added where it should be subtracted, one table step out on one wing',
     '        const double distance = std::abs ((double) i - offset) * tableScale;',
     '        const double distance = std::abs ((double) i + offset) * tableScale;'),

    ('truncation not floor', 'a negative read position truncating towards zero',
     '    const double floored = std::floor (position);',
     '    const double floored = (double) (long long) position;'),

    ('no between entry interpolation', 'the nearest table entry rather than between two',
     '        const double tap = (double) kernel.taps[(std::size_t) entry]\n                         + eta * (double) kernel.differences[(std::size_t) entry];',
     '        const double tap = (double) kernel.taps[(std::size_t) entry];'),

    ('eta fixed at a half', 'the between entry fraction not read from the position',
     '        const double eta = distance - (double) entry;',
     '        const double eta = 0.5;'),

    ('one tap short at the start', 'the first tap of the left wing dropped',
     '    for (int i = -half + 1; i <= half; ++i)',
     '    for (int i = -half + 2; i <= half; ++i)'),

    ('one tap short at the end', 'the last tap of the right wing dropped',
     '    for (int i = -half + 1; i <= half; ++i)',
     '    for (int i = -half + 1; i <= half - 1; ++i)'),

    ('table read past the end', 'the difference table read one entry too far',
     '        if (entry >= count - 1)',
     '        if (entry >= count)'),

    ('buffer read past the end', 'the source buffer read one sample too far',
     '        if (sample < 0 || sample >= frames)',
     '        if (sample < 0 || sample > frames)'),

    ('centre off by one', 'the read centred on the next sample',
     '    const int    centre  = (int) floored;',
     '    const int    centre  = (int) floored + 1;'),

    ('right channel from the left', 'both channels taken from the left buffer',
     '        sumRight += tap * (double) sourceRight[sample];',
     '        sumRight += tap * (double) sourceLeft[sample];'),

    ('outside is not silence', 'samples beyond the file taken from its nearest edge instead of as silence',
     '        const int sample = centre + i;\n\n        if (sample < 0 || sample >= frames)\n            continue;',
     '        int sample = centre + i;\n\n        sample = sample < 0 ? 0 : (sample >= frames ? frames - 1 : sample);'),

]

# A mutation that survives has to be shown to be a genuine no operation rather
# than a gap, and the reason goes here so a later night does not have to work it
# out again. The harness fails if one of these is ever caught, because that
# means the reason has stopped being true.
EXPECTED_SURVIVORS = {
    "differences before normalising":
        "measured, and it is a GAP rather than a no operation, which is why it "
        "is written here in full. Building the difference table from the "
        "unnormalised taps makes the two tables describe different kernels, "
        "which is the third of the three conditions the header calls out as "
        "breaking silently. It moves the central residual from -109.06 dB to "
        "-109.05 dB. It cannot be caught at any tolerance this file could "
        "honestly set, because the normalisation itself is only worth -132.8 "
        "dB and the residual threshold is -100: a test tight enough to see "
        "this would be pinning a figure 23 dB inside the specification and "
        "would fail on another compiler's libm. So the condition is correct, "
        "the code honours it, and nothing checks it. Said plainly because an "
        "unrecorded gap is how a real one hides next time.",

    "truncation not floor":
        "measured by an audit at -144.5 dB worst single sample difference, "
        "over positions from -60 to +140 reading a 10 kHz tone at 44.1 into "
        "48. The mechanism is that |i - offset| is the true distance to a "
        "sample whichever way the position was rounded, so truncating only "
        "shifts the loop window by one and the tap that falls off the far end "
        "is worth about 1e-7. The header's comment at that line says this "
        "rather than calling flooring a correctness requirement, which an "
        "earlier draft did: that is the Tuner::InputRing shape CLAUDE.md "
        "already records, a margin written up as a requirement.",

    "window unnormalised":
        "dividing the window by I0(beta) is one constant applied to the whole "
        "kernel, and the per phase normalisation that follows divides it out "
        "again. It is a true no operation, and the fact that it is one is "
        "itself a demonstration that the normalisation does what it says.",

    "window off by one":
        "it changes the half width by one entry in 24577, so the window moves "
        "by about four parts in a hundred thousand at its edges and by less "
        "everywhere else. Nothing in the audio band can see it.",

    "bessel loose stop":
        "at this beta the series reaches a relative 1e-6 in about twenty terms, "
        "and a window accurate to a part in a million still measures past 120 "
        "dB in the transform check. This mutation spends most of that check's "
        "1.6 dB of margin without crossing it. It is left as a survivor rather "
        "than chased, because the figure 1e-19 is not a constant anybody chose "
        "for a reason: what matters is that the criterion is RELATIVE rather "
        "than a fixed term count, and 'bessel fixed ten terms' shows that a "
        "fixed count is caught at -92.6 dB.",

    "no per phase normalisation":
        "measured, the raw table's worst per phase DC gain error is 2.29e-7, "
        "which is -132.8 dB, against a specification of 120 dB. So removing "
        "the normalisation does not break any property the header claims, and "
        "the header says exactly that: at this length the step is a nicety "
        "that costs nothing at build time rather than the necessity it was in "
        "Fx/Oversampler.h, where the same error measured five parts in a "
        "million. A test tolerance tight enough to catch this would be pinning "
        "a figure 12 dB inside the specification, which is a worse test and "
        "not a better one. ClipResampleProbe.cpp measures both numbers so the "
        "claim can be rechecked.",

    "normalise by the wrong phase":
        "same reason as 'no per phase normalisation'. Every phase sum is "
        "within 2.29e-7 of one, so dividing an entry by a different phase's "
        "sum is an error of that size.",

    "normalise by phase zero":
        "same reason as 'no per phase normalisation'.",

    "last difference not zeroed":
        "entry count - 1 is never read, so whatever is stored in its difference "
        "is never multiplied by anything. **The GUARD is what prevents that, "
        "not the loop bound**, and an earlier version of this entry said the "
        "opposite: it claimed the loop stops short of the end of the table. It "
        "does not. `half` is a ceiling, so at any step where crossings / scale "
        "is not a whole number the loop overshoots, by up to 354 entries at a "
        "step of 1.088435. An audit measured that. So this survivor is evidence "
        "that the guard works and evidence of nothing about the loop.",

    "table read past the end":
        "widening the guard from count - 1 to count admits exactly one more "
        "entry, count - 1, which is in bounds and holds exactly zero: it sits a "
        "whole number of samples from the centre, where the sinc is written as "
        "its exact value rather than evaluated, and its difference is pinned to "
        "zero. So the extra tap multiplies the source by nothing. **Not because "
        "the index cannot be reached**, which an earlier version of this entry "
        "said and an audit disproved: the loop reaches 24931 against a table of "
        "24577 at a step of 1.088435, which is why the guard is load bearing "
        "and says so at the line.",
}

def run_one(index):
    name, breaks, old, new = MUTATIONS[index]

    with tempfile.TemporaryDirectory() as work:
        include = os.path.join(work, "include")
        os.makedirs(include)

        source = open(HEADER).read()

        if source.count(old) != 1:
            return (index, name, breaks, "ANCHOR",
                    "the text to mutate appears %d times" % source.count(old))

        open(os.path.join(include, "ClipResampler.h"), "w").write(source.replace(old, new, 1))

        # The mutated header first, the real Source second, so everything else
        # the test includes is the real thing.
        binary = os.path.join(work, "mutant")
        compile_result = subprocess.run(
            [os.environ.get("CXX", "g++")] + FLAGS
            + ["-I", os.path.join(ROOT, "Tests", "juce-stub"),
               "-I", include, "-I", os.path.join(ROOT, "Source"),
               "-o", binary, TEST],
            capture_output=True, text=True)

        if compile_result.returncode != 0:
            # A mutation that will not compile is caught, in the sense that it
            # could never ship, but it is recorded separately because the test
            # is not what noticed.
            return (index, name, breaks, "NO BUILD", compile_result.stderr.strip().splitlines()[-1:] or [""])

        env = dict(os.environ)
        env["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=0"
        env["UBSAN_OPTIONS"] = "print_stacktrace=1"

        try:
            run_result = subprocess.run([binary], capture_output=True, text=True,
                                        timeout=900, env=env)
        except subprocess.TimeoutExpired:
            return (index, name, breaks, "CAUGHT", "the test did not finish")

        if run_result.returncode != 0:
            first = [line for line in run_result.stdout.splitlines() if "FAIL" in line]
            detail = first[0].strip() if first else "exited %d" % run_result.returncode
            return (index, name, breaks, "CAUGHT", detail)

        return (index, name, breaks, "SURVIVED", "")


def main():
    wanted = [int(a) for a in sys.argv[1:]] or list(range(len(MUTATIONS)))

    if shutil.which(os.environ.get("CXX", "g++")) is None:
        print("no compiler")
        return 1

    results = []

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        for result in pool.map(run_one, wanted):
            results.append(result)
            index, name, breaks, verdict, detail = result
            print("%-10s %2d  %-38s  %s" % (verdict, index, name, detail if isinstance(detail, str) else detail))
            sys.stdout.flush()

    caught = sum(1 for r in results if r[3] == "CAUGHT")
    nobuild = sum(1 for r in results if r[3] == "NO BUILD")
    survived = [r for r in results if r[3] == "SURVIVED"]
    anchor = [r for r in results if r[3] == "ANCHOR"]

    print()
    print("%d mutations: %d caught by the test, %d would not compile, %d survived, %d anchors stale"
          % (len(results), caught, nobuild, len(survived), len(anchor)))

    unexpected = [r for r in survived if r[1] not in EXPECTED_SURVIVORS]

    for r in survived:
        if r[1] in EXPECTED_SURVIVORS:
            print("  expected survivor  %2d  %s" % (r[0], r[1]))
            print("                         because %s" % EXPECTED_SURVIVORS[r[1]])
        else:
            print("  UNEXPECTED SURVIVOR  %2d  %s (%s)" % (r[0], r[1], r[2]))

    # The other direction: an expected survivor that is now caught means the
    # reason written above it has stopped being true, or the test has grown to
    # cover it, and either way this file is out of date.
    ran = {MUTATIONS[r[0]][0] for r in results}
    caught_names = {r[1] for r in results if r[3] == "CAUGHT"}

    now_caught = sorted(name for name in EXPECTED_SURVIVORS
                        if name in ran and name in caught_names)

    for name in now_caught:
        print("  NO LONGER A NO-OP    %s" % name)
        print("                       it was expected to survive because %s"
              % EXPECTED_SURVIVORS[name])

    for r in anchor:
        print("  STALE ANCHOR         %2d  %s" % (r[0], r[1]))

    if not unexpected and not anchor and not now_caught:
        print()
        print("Every mutation was caught, or survived for a reason this file records.")

    return 0 if not unexpected and not anchor and not now_caught else 1


if __name__ == "__main__":
    sys.exit(main())
