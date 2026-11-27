#!/usr/bin/env python3
"""
Mutation testing for Source/SpectrumAnalyser.h against Tests/SpectrumAnalyserTest.cpp.

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

    python3 Research/spectrum-mutations.py            all of them
    python3 Research/spectrum-mutations.py 12 13      just those two
"""

import concurrent.futures
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "Source", "SpectrumAnalyser.h")
TEST = os.path.join(ROOT, "Tests", "SpectrumAnalyserTest.cpp")

FLAGS = [
    "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter",
    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
    "-fno-sanitize-recover=undefined", "-D_GLIBCXX_ASSERTIONS", "-pthread",
]

# (short name, what it breaks, text to find, text to put there)
MUTATIONS = [
    # ---- band geometry ----
    ("odd formula everywhere", "one sixth octave bands land a semitone out",
     """    const double exponent = (bandsPerOctave % 2 != 0)
                                ? (double) x / b
                                : (2.0 * (double) x + 1.0) / (2.0 * b);""",
     """    const double exponent = (double) x / b;"""),

    ("even formula everywhere", "one third octave bands land a half band out",
     """    const double exponent = (bandsPerOctave % 2 != 0)
                                ? (double) x / b
                                : (2.0 * (double) x + 1.0) / (2.0 * b);""",
     """    const double exponent = (2.0 * (double) x + 1.0) / (2.0 * b);"""),

    ("base two", "G = 2 instead of 10^(3/10)",
     "inline double octaveRatio() noexcept { return std::pow (10.0, 0.3); }",
     "inline double octaveRatio() noexcept { return 2.0; }"),

    ("reference frequency", "the whole display shifted",
     "inline constexpr double referenceFrequency = 1000.0;",
     "inline constexpr double referenceFrequency = 440.0;"),

    ("edge exponent", "edges at G^(1/b) instead of G^(1/(2b))",
     "    return std::pow (octaveRatio(), 1.0 / (2.0 * (double) bandsPerOctave));",
     "    return std::pow (octaveRatio(), 1.0 / (double) bandsPerOctave);"),

    ("edges swapped", "lower and upper the other way round",
     """inline double lowerBandEdge (double midBand, int bandsPerOctave) noexcept
{
    return midBand / edgeRatio (bandsPerOctave);
}""",
     """inline double lowerBandEdge (double midBand, int bandsPerOctave) noexcept
{
    return midBand * edgeRatio (bandsPerOctave);
}"""),

    ("bandwidth as 1/b", "the obvious wrong reading of a bandwidth designator",
     """    const double r = edgeRatio (bandsPerOctave);
    return r - 1.0 / r;""",
     """    return 1.0 / (double) bandsPerOctave;"""),

    ("base two bandwidth", "2^(1/6) - 2^(-1/6), which is close enough to look right",
     """    const double r = edgeRatio (bandsPerOctave);
    return r - 1.0 / r;""",
     """    const double r = std::pow (2.0, 1.0 / (2.0 * (double) bandsPerOctave));
    return r - 1.0 / r;"""),

    ("built in div and mod", "the negative band index trap in the ISO 266 labels",
     """    int decadeIndex = x / 10;
    int within      = x % 10;

    if (within < 0)
    {
        within += 10;
        --decadeIndex;
    }""",
     """    const int decadeIndex = x / 10;
    const int within      = std::abs (x % 10);"""),

    ("nominal table entry", "3150 written as 3200, which looks like a round number",
     "3150.0, 4000.0, 5000.0, 6300.0, 8000.0 };",
     "3200.0, 4000.0, 5000.0, 6300.0, 8000.0 };"),

    ("nominal for every bandwidth", "an invented preferred series for sixths",
     """    if (bandsPerOctave != 3)
        return 0.0;""",
     """    if (bandsPerOctave < 1)
        return 0.0;"""),

    ("no Nyquist limit", "the top band truncated and reading low",
     "    const double highestEdge = std::min (audioRangeUpperEdge(), 0.5 * sampleRate);",
     "    const double highestEdge = audioRangeUpperEdge();"),

    ("no edge stitching", "hairline gaps between the bands",
     """    for (std::size_t i = 1; i < result.size(); ++i)
        result[i].lower = result[i - 1].upper;

""",
     ""),

    ("range by centres", "the band the display is named after dropped",
     "        if (band.lower * slack >= lowestEdge && band.upper <= highestEdge * slack)",
     "        if (band.centre * slack >= 20.0 && band.upper <= highestEdge * slack)"),

    # ---- the window ----
    ("symmetric Hann", "Harris's constants stop being exact",
     "        window[(std::size_t) i] = 0.5 - 0.5 * std::cos (twoPi * (double) i / (double) size);",
     "        window[(std::size_t) i] = 0.5 - 0.5 * std::cos (twoPi * (double) i / (double) (size - 1));"),

    ("window flipped", "a window that rises to one at the ends",
     "        window[(std::size_t) i] = 0.5 - 0.5 * std::cos (twoPi * (double) i / (double) size);",
     "        window[(std::size_t) i] = 0.5 + 0.5 * std::cos (twoPi * (double) i / (double) size);"),

    ("coherent gain by N-1", "a constant that depends on the size",
     "    return sum / (double) window.size();",
     "    return sum / (double) (window.size() - 1);"),

    ("noise bandwidth inverted", "the reciprocal of the right constant",
     "    return (double) window.size() * sumOfSquares / (sum * sum);",
     "    return (sum * sum) / ((double) window.size() * sumOfSquares);"),

    # ---- the normalisation ----
    ("window sum normalisation", "the 1.76 dB the research recommended",
     "        const double scale   = 2.0 / ((double) fftSize * windowSumOfSquares);",
     "        const double scale   = 2.0 * 1.5 / ((double) fftSize * windowSumOfSquares);"),

    ("per channel reference", "3.01 dB, the fault the audit found: one leg taken as full scale",
     "inline constexpr double fullScaleReferenceDb = 0.0;",
     "inline constexpr double fullScaleReferenceDb = 3.0102999566398120;"),

    ("RMS reference", "3.01 dB the other way, from AES17's convention for a sine",
     "inline constexpr double fullScaleReferenceDb = 0.0;",
     "inline constexpr double fullScaleReferenceDb = -3.0102999566398120;"),

    ("interleaved capacity not doubled", "a ring half the size the two legs need",
     "    return 2 * ringCapacity (largestBlockSamples);",
     "    return ringCapacity (largestBlockSamples);"),

    ("no single sided factor", "the negative frequencies left out",
     "        const double scale   = 2.0 / ((double) fftSize * windowSumOfSquares);",
     "        const double scale   = 1.0 / ((double) fftSize * windowSumOfSquares);"),

    ("mirror on every bin", "a doubled direct current and Nyquist bin",
     "            const double mirror = (k == 0 || k == lastBin) ? 0.5 : 1.0;",
     "            const double mirror = 1.0;"),

    ("mirror on no bin", "every bin halved",
     "            const double mirror = (k == 0 || k == lastBin) ? 0.5 : 1.0;",
     "            const double mirror = 0.5;"),

    ("no window", "a rectangular window, so every tone leaks across the display",
     "            scratch[(std::size_t) i] = (float) ((double) channel[i] * window[(std::size_t) i]);",
     "            scratch[(std::size_t) i] = channel[i];"),

    # ---- the bin to band weights ----
    ("bin spans from its centre", "every bin half a bin high",
     """                const double binLower = ((double) k - 0.5) * df;
                const double binUpper = ((double) k + 0.5) * df;""",
     """                const double binLower = (double) k * df;
                const double binUpper = ((double) k + 1.0) * df;"""),

    ("hard include on the centre", "the step a fractional weight exists to remove",
     """                const double w = overlap > 0.0 ? overlap / df : 0.0;""",
     """                const double centre = (double) k * df;
                const double w = (centre >= band.lower && centre < band.upper) ? 1.0 : 0.0;
                (void) overlap;"""),

    ("weights not clamped", "a negative weight from a bin outside the band",
     "                const double w = overlap > 0.0 ? overlap / df : 0.0;",
     "                const double w = overlap / df;"),

    ("no half bin reach", "the bins at a band's edges left out",
     """            int first = (int) std::ceil (band.lower / df - 0.5);
            int last  = (int) std::floor (band.upper / df + 0.5);""",
     """            int first = (int) std::ceil (band.lower / df);
            int last  = (int) std::floor (band.upper / df);"""),

    ("first bin not clamped", "a read before the start of the spectrum",
     "            first = std::max (0, first);",
     "            first = first;"),

    ("last bin not clamped", "a read past Nyquist",
     "            last  = std::min (lastBin, last);",
     "            last  = last;"),

    # ---- the refusal ----
    ("resolve anything", "a band narrower than a bin shown reading low",
     "        return bandBins[(std::size_t) band].totalWeight >= 1.0;",
     "        return bandBins[(std::size_t) band].totalWeight > 0.0;"),

    ("resolve at four bins", "the bottom two octaves blanked",
     "        return bandBins[(std::size_t) band].totalWeight >= 1.0;",
     "        return bandBins[(std::size_t) band].totalWeight >= 4.0;"),

    # ---- the ballistics ----
    ("linear smoothing coefficient", "the obvious approximation to 1 - exp(-t/tau)",
     "        const double alpha = 1.0 - std::exp (-frameSeconds / tau);",
     "        const double alpha = std::min (1.0, frameSeconds / tau);"),

    ("smoothed in the magnitude domain", "twice the decay rate, matching no meter",
     """            smoothedPower[i] = first ? power
                                     : smoothedPower[i] + alpha * (power - smoothedPower[i]);""",
     """            const double wasMagnitude = std::sqrt (std::max (0.0, smoothedPower[i]));
            const double isMagnitude  = std::sqrt (std::max (0.0, power));
            const double blended      = first ? isMagnitude
                                              : wasMagnitude + alpha * (isMagnitude - wasMagnitude);
            smoothedPower[i] = blended * blended;"""),

    ("time constants swapped", "fast is slow and slow is fast",
     """    return b == Ballistics::slow ? slowTimeConstantSeconds : fastTimeConstantSeconds;""",
     """    return b == Ballistics::slow ? fastTimeConstantSeconds : slowTimeConstantSeconds;"""),

    ("fast time constant", "125 ms written as 250",
     "inline constexpr double fastTimeConstantSeconds = 0.125;",
     "inline constexpr double fastTimeConstantSeconds = 0.250;"),

    ("slow time constant", "1 s written as 2",
     "inline constexpr double slowTimeConstantSeconds = 1.0;",
     "inline constexpr double slowTimeConstantSeconds = 2.0;"),

    ("no first frame seeding", "a time constant of climbing every time the view opens",
     """            smoothedPower[i] = first ? power
                                     : smoothedPower[i] + alpha * (power - smoothedPower[i]);""",
     """            smoothedPower[i] = smoothedPower[i] + alpha * (power - smoothedPower[i]);"""),

    ("decay rate expression", "20/ln(10) instead of 10/ln(10)",
     "    return timeConstant > 0.0 ? 10.0 / std::log (10.0) / timeConstant : 0.0;",
     "    return timeConstant > 0.0 ? 20.0 / std::log (10.0) / timeConstant : 0.0;"),

    # ---- the display conventions ----
    ("tilt defaults to the documented 4.5", "the double count this whole file is about",
     "    double                             tilt       = 0.0;",
     "    double                             tilt       = documentedPerBinTiltDbPerOctave;"),

    ("tilt in decades", "log10 where log2 was meant",
     "            db += tilt * std::log2 (centre / tiltPivotFrequency);",
     "            db += tilt * std::log10 (centre / tiltPivotFrequency);"),

    ("tilt pivot", "pivoting at 100 Hz instead of 1 kHz",
     "inline constexpr double tiltPivotFrequency = 1000.0;",
     "inline constexpr double tiltPivotFrequency = 100.0;"),

    ("tilt sign", "the display leaning the other way",
     "            db += tilt * std::log2 (centre / tiltPivotFrequency);",
     "            db -= tilt * std::log2 (centre / tiltPivotFrequency);"),

    ("preference tilt", "the residual written as the whole 4.5",
     """inline constexpr double preferenceTiltDbPerOctave
    = documentedPerBinTiltDbPerOctave - perBinPinkTiltDbPerOctave;""",
     """inline constexpr double preferenceTiltDbPerOctave = documentedPerBinTiltDbPerOctave;"""),

    # ---- the channels ----
    ("right channel ignored", "a mono reading of a stereo mix",
     """        if (right != nullptr)
            accumulate (right);""",
     """        if (right == nullptr)
            accumulate (left);"""),

    ("right channel twice", "3 dB up on everything stereo",
     """        if (right != nullptr)
            accumulate (right);""",
     """        if (right != nullptr)
        {
            accumulate (right);
            accumulate (right);
        }"""),

    # ---- the refusals ----
    ("non-finite samples accepted", "one NaN block killing the display for the session",
     """    for (int i = 0; i < n; ++i)
            if (! std::isfinite (x[i]))
                return false;

        return true;""",
     """    (void) n; (void) x;
        return true;"""),

    ("right channel not checked", "the same thing through the other channel",
     """        if (right != nullptr && ! allFinite (right, numSamples))
            return false;

""",
     ""),

    ("frame interval unchecked", "a zero or negative interval reaching the smoother",
     """        if (! (frameSeconds > 0.0) || ! std::isfinite (frameSeconds))
            return false;

""",
     ""),

    ("short frames accepted", "a read past the end of the caller's buffer",
     "        if (! isConfigured() || left == nullptr || numSamples != fftSize)",
     "        if (! isConfigured() || left == nullptr || numSamples <= 0)"),

    ("any transform size", "a transform silently running at another length",
     """        if (newFftSize < 64 || (newFftSize & (newFftSize - 1)) != 0)
            return;""",
     """        if (newFftSize < 64)
            return;"""),

    ("non-finite tilt accepted", "every band reading not a number",
     "        tilt = std::isfinite (t) ? t : 0.0;",
     "        tilt = t;"),

    # ---- the hold ----
    ("hold follows the level", "a peak hold that holds nothing",
     "            heldPower[i] = std::max (heldPower[i], smoothedPower[i]);",
     "            heldPower[i] = smoothedPower[i];"),

    ("hold reset to the level", "a reset that cannot be seen to have happened",
     """    void resetHold() noexcept
    {
        std::fill (heldPower.begin(), heldPower.end(), 0.0);
    }""",
     """    void resetHold() noexcept
    {
        heldPower = smoothedPower;
    }"""),

    ("clear forgets nothing", "a reconfigured display keeping its old readings",
     """    void clear() noexcept
    {
        std::fill (smoothedPower.begin(), smoothedPower.end(), 0.0);
        std::fill (heldPower.begin(), heldPower.end(), 0.0);
        frames = 0;
    }""",
     """    void clear() noexcept
    {
        frames = 0;
    }"""),

    # ---- the ring capacity ----
    ("ring capacity halved", "a figure that still looks generous and accepts torn frames",
     "    return largestTransformSize + 4 * largestBlockSamples;",
     "    return largestTransformSize + largestBlockSamples / 2;"),
]


# The mutations that are expected to survive, with what each one relies on.
#
# Every one of them is a genuine no-op: the code it breaks is correct and is
# also unreachable, or is enforced again somewhere else. They are listed here
# rather than described in a comment, and the check below works BOTH WAYS: a new
# survivor fails, and so does an expected survivor that has started being
# caught. That is the discipline VoiceTest.h uses for its list of unreached
# modulation destinations, and for the same reason. A list of known gaps that
# nothing checks quietly becomes a lie.
EXPECTED_SURVIVORS = {
    "mirror on every bin":
        "no resolved band reaches the direct current or the Nyquist bin, which the "
        "test asserts at every rate and transform size",
    "weights not clamped":
        "the run of bins is chosen so the overlap over it is never negative",
    "first bin not clamped":
        "band.lower is positive, so ceil (lower/df - 0.5) is never below zero",
    "last bin not clamped":
        "no band is allowed above Nyquist, so floor (upper/df + 0.5) <= lastBin",
    "any transform size":
        "RealFFT::setSize refuses a size that is not a power of two and leaves "
        "itself unusable, which the getSize check then catches",
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

        open(os.path.join(include, "SpectrumAnalyser.h"), "w").write(source.replace(old, new, 1))

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
