#!/usr/bin/env python3
"""Mutation harness for Source/WarpMap.h against Tests/WarpMapTest.cpp.

    python3 Research/warp-mutations.py            every mutation
    python3 Research/warp-mutations.py G2 R3       only those


Compiles each broken copy with exactly the flags Tests/run.sh uses, including
the sanitizers, because a plain -O2 build calls an out of bounds read a
survivor.
"""
import os, subprocess, sys, tempfile, shutil, concurrent.futures

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = tempfile.mkdtemp(prefix="warpmut_")
HDR  = os.path.join(REPO, "Source/WarpMap.h")
TEST = os.path.join(REPO, "Tests/WarpMapTest.cpp")

FLAGS = ["-std=c++20","-O1","-g","-Wall","-Wextra","-Wno-unused-parameter",
         "-fsanitize=address,undefined","-fno-omit-frame-pointer",
         "-fno-sanitize-recover=undefined","-D_GLIBCXX_ASSERTIONS","-pthread"]

ORIGINAL = open(HDR).read()

# (id, area, description, old, new)   old must occur exactly once unless count given
M = []
def m(i, area, desc, old, new, count=1):
    M.append((i, area, desc, old, new, count))

# ---- normaliseWarpMarkers ----
m("N1","normalise","the filter that drops a marker which is not a number is gone",
  """    markers.erase (std::remove_if (markers.begin(), markers.end(),
                                   [] (const WarpMarker& m)
                                   {
                                       return ! (std::isfinite (m.beat) && std::isfinite (m.source));
                                   }),
                   markers.end());

""","")
m("N2","normalise","only the beat is checked for being a number, not the source",
  "return ! (std::isfinite (m.beat) && std::isfinite (m.source));",
  "return ! (std::isfinite (m.beat));")
m("N3","normalise","the finite test is inverted, so only the broken markers survive",
  "return ! (std::isfinite (m.beat) && std::isfinite (m.source));",
  "return (std::isfinite (m.beat) && std::isfinite (m.source));")
m("N4","normalise","stable_sort becomes sort, so which of two stacked markers survives is unspecified",
  "std::stable_sort (markers.begin(), markers.end(),","std::sort (markers.begin(), markers.end(),")
m("N5","normalise","sorted by source position instead of by beat",
  "{ return a.beat < b.beat; });","{ return a.source < b.source; });")
m("N6","normalise","sorted descending",
  "{ return a.beat < b.beat; });","{ return a.beat > b.beat; });")
m("N7","normalise","the left edge stops pinning the source, so a marker before the offset survives",
  "double prevBeat = 0.0, prevSource = offset;\n    std::vector<WarpMarker> kept;",
  "double prevBeat = 0.0, prevSource = 0.0;\n    std::vector<WarpMarker> kept;")
m("N8","normalise","the left edge stops pinning the beat, so a marker before the clip survives",
  "double prevBeat = 0.0, prevSource = offset;\n    std::vector<WarpMarker> kept;",
  "double prevBeat = -1.0e300, prevSource = offset;\n    std::vector<WarpMarker> kept;")
m("N9","normalise","a marker is dropped only when it fails in both coordinates, not either",
  "if (m.beat < prevBeat + kMinWarpBeatSpan || m.source < prevSource + kMinWarpSourceSpan)",
  "if (m.beat < prevBeat + kMinWarpBeatSpan && m.source < prevSource + kMinWarpSourceSpan)")
m("N10","normalise","the beat span is dropped, so two markers on the same beat both survive",
  "if (m.beat < prevBeat + kMinWarpBeatSpan ||","if (m.beat < prevBeat ||")
m("N11","normalise","the source span is dropped, so two markers on one sample both survive",
  "|| m.source < prevSource + kMinWarpSourceSpan)","|| m.source < prevSource)")
m("N12","normalise","the beat test becomes non strict",
  "if (m.beat < prevBeat + kMinWarpBeatSpan ||","if (m.beat <= prevBeat + kMinWarpBeatSpan ||")
m("N13","normalise","the running predecessor advances even for a marker that was dropped",
  """        if (m.beat < prevBeat + kMinWarpBeatSpan || m.source < prevSource + kMinWarpSourceSpan)
            continue;
        kept.push_back (m);
        prevBeat   = m.beat;
        prevSource = m.source;""",
  """        const bool usable = ! (m.beat < prevBeat + kMinWarpBeatSpan || m.source < prevSource + kMinWarpSourceSpan);
        prevBeat   = m.beat;
        prevSource = m.source;
        if (! usable)
            continue;
        kept.push_back (m);""")
m("N14","normalise","the cap is off by one",
  "if (kept.size() >= kMaxWarpMarkers)","if (kept.size() > kMaxWarpMarkers)")
m("N15","normalise","the cap on the number of markers is gone",
  """
        if (kept.size() >= kMaxWarpMarkers)
            break;
""","")
m("N16","normalise","the cap is raised to something a project file can exceed anyway",
  "inline constexpr size_t kMaxWarpMarkers = 4096;","inline constexpr size_t kMaxWarpMarkers = 1u << 30;")
m("N17","normalise","the normalised list is never written back",
  "    markers.swap (kept);","    (void) kept;")
m("N18","normalise","the minimum beat span is loosened to nothing",
  "inline constexpr double kMinWarpBeatSpan   = 1.0e-4;",
  "inline constexpr double kMinWarpBeatSpan   = 1.0e-300;")
m("N19","normalise","the minimum source span is loosened to nothing",
  "inline constexpr double kMinWarpSourceSpan = 1.0e-5;",
  "inline constexpr double kMinWarpSourceSpan = 1.0e-300;")
m("N20","normalise","the two span constants are used for each other's coordinate",
  "if (m.beat < prevBeat + kMinWarpBeatSpan || m.source < prevSource + kMinWarpSourceSpan)",
  "if (m.beat < prevBeat + kMinWarpSourceSpan || m.source < prevSource + kMinWarpBeatSpan)")

# ---- warpFallbackSlope / warpStretchForSlope ----
m("F1","fallback","beats per second instead of seconds per beat",
  "const double secondsPerBeat = 60.0 / std::max (1.0e-6, bpm);",
  "const double secondsPerBeat = std::max (1.0e-6, bpm) / 60.0;")
m("F2","fallback","the stretch ratio multiplies instead of dividing",
  "return secondsPerBeat / std::max (1.0e-9, stretch);",
  "return secondsPerBeat * std::max (1.0e-9, stretch);")
m("F3","fallback","the guard on a zero tempo is gone",
  "const double secondsPerBeat = 60.0 / std::max (1.0e-6, bpm);",
  "const double secondsPerBeat = 60.0 / bpm;")
m("F4","fallback","the wrong number of seconds in a minute",
  "const double secondsPerBeat = 60.0 / std::max (1.0e-6, bpm);",
  "const double secondsPerBeat = 120.0 / std::max (1.0e-6, bpm);")
m("W1","stretch","the inverse of warpFallbackSlope multiplies by the rate",
  "return 60.0 / std::max (1.0e-6, bpm) / std::max (1.0e-12, slope);",
  "return 60.0 / std::max (1.0e-6, bpm) * std::max (1.0e-12, slope);")
m("W2","stretch","the inverse is upside down",
  "return 60.0 / std::max (1.0e-6, bpm) / std::max (1.0e-12, slope);",
  "return std::max (1.0e-12, slope) * std::max (1.0e-6, bpm) / 60.0;")

# ---- warpSourceAtBeat ----
m("S1","sourceAtBeat","the segment origin is dropped from the extrapolation",
  """        if (beat < b1 || i + 1 == markers.size())
            return s0 + (beat - b0) * slope;""",
  """        if (beat < b1 || i + 1 == markers.size())
            return s0 + beat * slope;""")
m("S2","sourceAtBeat","the segment boundary test becomes non strict",
  "        if (beat < b1 || i + 1 == markers.size())\n            return s0 + (beat - b0) * slope;",
  "        if (beat <= b1 || i + 1 == markers.size())\n            return s0 + (beat - b0) * slope;")
m("S3","sourceAtBeat","the segment boundary test is the wrong way round",
  "        if (beat < b1 || i + 1 == markers.size())\n            return s0 + (beat - b0) * slope;",
  "        if (beat > b1 || i + 1 == markers.size())\n            return s0 + (beat - b0) * slope;")
m("S4","sourceAtBeat","the last segment no longer carries on past the last marker",
  "        if (beat < b1 || i + 1 == markers.size())\n            return s0 + (beat - b0) * slope;",
  "        if (beat < b1)\n            return s0 + (beat - b0) * slope;")
m("S5","sourceAtBeat","the rate is taken over the wrong span",
  """        const double b1 = markers[i].beat, s1 = markers[i].source;
        const double slope = std::max (1.0e-12, (s1 - s0) / (b1 - b0));

        // The last segment's rate carries on past the last marker, and the""",
  """        const double b1 = markers[i].beat, s1 = markers[i].source;
        const double slope = std::max (1.0e-12, (s1 - s0) / b1);

        // The last segment's rate carries on past the last marker, and the""")
m("S6","sourceAtBeat","the clamp that keeps a rate positive is gone",
  "        const double slope = std::max (1.0e-12, (s1 - s0) / (b1 - b0));\n\n        // The last segment's rate",
  "        const double slope = (s1 - s0) / (b1 - b0);\n\n        // The last segment's rate")
m("S7","sourceAtBeat","an unwarped clip forgets its offset",
  "    if (markers.empty())\n        return offset + beat * fallbackSlope;",
  "    if (markers.empty())\n        return beat * fallbackSlope;")
m("S8","sourceAtBeat","the walk advances the beat but not the source",
  """        b0 = b1;
        s0 = s1;
    }

    return s0;   // not reachable: the loop always returns on its last pass""",
  """        b0 = b1;
    }

    return s0;   // not reachable: the loop always returns on its last pass""")
m("S9","sourceAtBeat","the extrapolation is anchored on the far marker instead of the near one (algebraically the same)",
  """        if (beat < b1 || i + 1 == markers.size())
            return s0 + (beat - b0) * slope;""",
  """        if (beat < b1 || i + 1 == markers.size())
            return s1 + (beat - b1) * slope;""")

# ---- warpBeatAtSource ----
m("B1","beatAtSource","the segment origin is dropped from the inverse",
  "        if (source < s1 || i + 1 == markers.size())\n            return b0 + (source - s0) / slope;",
  "        if (source < s1 || i + 1 == markers.size())\n            return b0 + source / slope;")
m("B2","beatAtSource","the inverse boundary test becomes non strict",
  "        if (source < s1 || i + 1 == markers.size())",
  "        if (source <= s1 || i + 1 == markers.size())")
m("B3","beatAtSource","the inverse multiplies by the rate instead of dividing",
  "            return b0 + (source - s0) / slope;",
  "            return b0 + (source - s0) * slope;")
m("B4","beatAtSource","an unwarped clip inverts by multiplying",
  "        return (source - offset) / std::max (1.0e-12, fallbackSlope);",
  "        return (source - offset) * std::max (1.0e-12, fallbackSlope);")
m("B5","beatAtSource","the inverse compares the source against a beat",
  "        if (source < s1 || i + 1 == markers.size())",
  "        if (source < b1 || i + 1 == markers.size())")
m("B6","beatAtSource","the last segment no longer carries on past the last marker",
  "        if (source < s1 || i + 1 == markers.size())",
  "        if (source < s1)")

# ---- warpSlopeAtBeat ----
m("L1","slopeAtBeat","the boundary test becomes non strict",
  """        if (beat < b1 || i + 1 == markers.size())
            return slope;""",
  """        if (beat <= b1 || i + 1 == markers.size())
            return slope;""")
m("L2","slopeAtBeat","the local rate is reported as the clip's fallback rate",
  """        if (beat < b1 || i + 1 == markers.size())
            return slope;""",
  """        if (beat < b1 || i + 1 == markers.size())
            return fallbackSlope;""")
m("L3","slopeAtBeat","the local rate is reported upside down",
  """        const double slope = std::max (1.0e-12, (s1 - s0) / (b1 - b0));

        if (beat < b1 || i + 1 == markers.size())
            return slope;""",
  """        const double slope = std::max (1.0e-12, (b1 - b0) / (s1 - s0));

        if (beat < b1 || i + 1 == markers.size())
            return slope;""")
m("L4","slopeAtBeat","an unwarped clip reports no rate at all",
  "    if (markers.empty())\n        return fallbackSlope;","    if (markers.empty())\n        return 0.0;")
m("L5","slopeAtBeat","the walk advances the beat but not the source",
  """        if (beat < b1 || i + 1 == markers.size())
            return slope;

        b0 = b1;
        s0 = s1;""",
  """        if (beat < b1 || i + 1 == markers.size())
            return slope;

        b0 = b1;""")

# ---- rebaseWarpMarkers ----
m("R1","rebase","the beat shift is applied the wrong way",
  "        m.beat -= beatShift;","        m.beat += beatShift;")
m("R2","rebase","the rebased list is normalised against the old left edge",
  "    normaliseWarpMarkers (markers, newOffset);\n}","    normaliseWarpMarkers (markers, 0.0);\n}")
m("R3","rebase","the rebased list is never normalised",
  "    normaliseWarpMarkers (markers, newOffset);\n}","    (void) newOffset;\n}")
m("R4","rebase","the shift is applied to the source position as well",
  "        m.beat -= beatShift;","        m.beat -= beatShift;\n        m.source -= beatShift;")

# ---- buildWarpSegments ----
m("G1","segments","the previous segment keeps the fallback rate instead of the measured one",
  "        out[out.size() - 1].slope = slope * scale;\n","")
m("G2","segments","a segment forgets the clip's position in the arrangement",
  "        out.push_back ({ startBeat + m.beat, m.source * scale, slope * scale });",
  "        out.push_back ({ m.beat, m.source * scale, slope * scale });")
m("G3","segments","the first segment's source is left unscaled",
  "    out.push_back ({ startBeat, offset * scale, std::max (1.0e-12, fallbackSlope) * scale });",
  "    out.push_back ({ startBeat, offset, std::max (1.0e-12, fallbackSlope) * scale });")
m("G4","segments","a marker's rate is left unscaled",
  "        out.push_back ({ startBeat + m.beat, m.source * scale, slope * scale });",
  "        out.push_back ({ startBeat + m.beat, m.source * scale, slope });")
m("G5","segments","a segment's rate is taken over the wrong span",
  "        const double slope = std::max (1.0e-12, (m.source - s0) / (m.beat - b0));",
  "        const double slope = std::max (1.0e-12, (m.source - s0) / m.beat);")
m("G6","segments","the walk advances the beat but not the source",
  "        b0 = m.beat;\n        s0 = m.source;","        b0 = m.beat;")
m("G7","segments","the guard on a zero source scale is gone",
  "    const double scale = std::max (1.0e-9, sourceScale);","    const double scale = sourceScale;")
m("G8","segments","the guard that keeps the fallback rate positive is gone",
  "    out.push_back ({ startBeat, offset * scale, std::max (1.0e-12, fallbackSlope) * scale });",
  "    out.push_back ({ startBeat, offset * scale, fallbackSlope * scale });")
m("G9","segments","the new segment is pushed before the previous one's rate is fixed",
  """        out[out.size() - 1].slope = slope * scale;
        out.push_back ({ startBeat + m.beat, m.source * scale, slope * scale });   // tail rate, replaced if more follow""",
  """        out.push_back ({ startBeat + m.beat, m.source * scale, slope * scale });   // tail rate, replaced if more follow
        out[out.size() - 1].slope = slope * scale;""")
m("G10","segments","the guaranteed first segment is only written when there are no markers",
  "    out.push_back ({ startBeat, offset * scale, std::max (1.0e-12, fallbackSlope) * scale });",
  "    if (markers.empty())\n        out.push_back ({ startBeat, offset * scale, std::max (1.0e-12, fallbackSlope) * scale });")

# ---- warpSourceAtSegment ----
m("A1","atSegment","the cursor advance test becomes strict",
  "    while (cursor + 1 < count && segments[cursor + 1].beat <= beat)",
  "    while (cursor + 1 < count && segments[cursor + 1].beat < beat)")
m("A2","atSegment","the cursor is never walked back, so looping the transport reads the wrong segment",
  "    while (cursor > 0 && segments[cursor].beat > beat)\n        --cursor;\n","")
m("A3","atSegment","the cursor is never walked forward",
  "    while (cursor + 1 < count && segments[cursor + 1].beat <= beat)\n        ++cursor;\n","")
m("A4","atSegment","the cursor clamp is off by one at the top",
  "    cursor = cursor < 0 ? 0 : (cursor >= count ? count - 1 : cursor);",
  "    cursor = cursor < 0 ? 0 : (cursor > count ? count : cursor);")
m("A5","atSegment","a negative cursor is no longer clamped",
  "    cursor = cursor < 0 ? 0 : (cursor >= count ? count - 1 : cursor);",
  "    cursor = cursor >= count ? count - 1 : cursor;")
m("A6","atSegment","the segment origin is dropped from the audio thread's read",
  "    return s.source + (beat - s.beat) * s.slope;",
  "    return s.source + beat * s.slope;")
m("A7","atSegment","an empty table is no longer refused",
  "    if (segments == nullptr || count <= 0)","    if (segments == nullptr || count < 0)")
m("A8","atSegment","the null table guard is gone",
  "    if (segments == nullptr || count <= 0)\n        return 0.0;\n\n","")
m("A9","atSegment","the walk back test becomes non strict and cannot terminate at the right place",
  "    while (cursor > 0 && segments[cursor].beat > beat)",
  "    while (cursor > 0 && segments[cursor].beat >= beat)")
m("A10","atSegment","the clamp is applied after the walk rather than before it",
  """    cursor = cursor < 0 ? 0 : (cursor >= count ? count - 1 : cursor);

    while (cursor + 1 < count && segments[cursor + 1].beat <= beat)
        ++cursor;
    while (cursor > 0 && segments[cursor].beat > beat)
        --cursor;""",
  """    while (cursor + 1 < count && segments[cursor + 1].beat <= beat)
        ++cursor;
    while (cursor > 0 && segments[cursor].beat > beat)
        --cursor;

    cursor = cursor < 0 ? 0 : (cursor >= count ? count - 1 : cursor);""")

def run_one(mut):
    mid, area, desc, old, new, count = mut
    if ORIGINAL.count(old) != count:
        return (mid, area, desc, "BADPATTERN", ORIGINAL.count(old))
    d = tempfile.mkdtemp(prefix="mut_", dir=WORK)
    try:
        open(os.path.join(d, "WarpMap.h"), "w").write(ORIGINAL.replace(old, new, count))
        exe = os.path.join(d, "t")
        c = subprocess.run(["g++", *FLAGS, "-I", os.path.join(REPO,"Tests/juce-stub"),
                            "-I", d, "-I", os.path.join(REPO,"Source"), "-o", exe, TEST],
                           capture_output=True, text=True, timeout=600)
        if c.returncode != 0:
            return (mid, area, desc, "CAUGHT-COMPILE", (c.stderr or "")[-300:])
        r = subprocess.run([exe], capture_output=True, text=True, timeout=1800,
                           env={**os.environ, "ASAN_OPTIONS":"detect_leaks=1:abort_on_error=0",
                                "UBSAN_OPTIONS":"print_stacktrace=1"})
        if r.returncode != 0:
            tail = [l for l in (r.stdout+r.stderr).splitlines() if "FAIL" in l or "ERROR" in l or "runtime error" in l]
            return (mid, area, desc, "CAUGHT", (tail[0][:200] if tail else "exit %d" % r.returncode))
        return (mid, area, desc, "SURVIVED", "")
    except subprocess.TimeoutExpired:
        return (mid, area, desc, "CAUGHT-TIMEOUT", "")
    finally:
        shutil.rmtree(d, ignore_errors=True)

if __name__ == "__main__":
    only = sys.argv[1:] 
    todo = [x for x in M if not only or x[0] in only]
    print("%d mutations" % len(todo), flush=True)
    res = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
        for r in ex.map(run_one, todo):
            res.append(r)
            print("%-5s %-12s %-10s %s" % (r[0], r[1], r[3], r[2]), flush=True)
            if r[3] == "BADPATTERN":
                print("        pattern count: %s" % (r[4],), flush=True)
    surv = [r for r in res if r[3] == "SURVIVED"]
    bad  = [r for r in res if r[3] == "BADPATTERN"]
    print("\n%d tried, %d caught, %d survived, %d bad patterns"
          % (len(res), len(res)-len(surv)-len(bad), len(surv), len(bad)))
    for r in surv:
        print("  SURVIVOR %-5s %-12s %s" % (r[0], r[1], r[2]))
