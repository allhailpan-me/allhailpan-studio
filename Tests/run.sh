#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Builds and runs every standalone test in this directory.
#
# JUCE cannot be compiled in a few seconds, and most of the logic that would be
# silently wrong here is arithmetic that does not need it. So anything ending
# in Test.cpp is expected to build against the headers in Source/ alone, with
# no JUCE and no CMake, and to exit non-zero when it finds a problem. CI runs
# this script on every push, which is what keeps these honest: a test nothing
# executes is a comment that takes longer to write.
#
# Sanitizers are on because two of the bugs these caught were an out of bounds
# read and an unsequenced expression, neither of which shows up as a wrong
# answer until the day it does.
#
# Files named *Probe.cpp are deliberately not run. They are measurements that
# informed a design decision, kept so the decision can be rechecked, and they
# need third party sources and minutes rather than seconds.
#
#   ./Tests/run.sh            from anywhere in the repository
# ---------------------------------------------------------------------------

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source_dir="$here/../Source"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

CXX="${CXX:-g++}"
flags=(-std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter
       -fsanitize=address,undefined -fno-omit-frame-pointer
       -fno-sanitize-recover=undefined)

# Halting on the first undefined behaviour rather than carrying on means the
# exit code is trustworthy. Leak detection stays on; these are small programs
# and a leak here is a real mistake.
export ASAN_OPTIONS="detect_leaks=1:abort_on_error=0"
export UBSAN_OPTIONS="print_stacktrace=1"

shopt -s nullglob
tests=("$here"/*Test.cpp)
shopt -u nullglob

if [ ${#tests[@]} -eq 0 ]; then
    echo "No tests found in $here"
    exit 1
fi

failed=0

for test_file in "${tests[@]}"; do
    name="$(basename "$test_file" .cpp)"
    echo "=============================================================="
    echo "$name"
    echo "=============================================================="

    # juce-stub comes first so a header under test can include its JUCE module
    # and get nothing, leaving the test to declare the few types it really
    # uses. See Tests/juce-stub/README.md.
    if ! "$CXX" "${flags[@]}" -I "$here/juce-stub" -I "$source_dir" \
            -o "$work/$name" "$test_file"; then
        echo "::error::$name failed to compile"
        failed=1
        continue
    fi

    if ! "$work/$name"; then
        echo "::error::$name failed"
        failed=1
    fi
    echo
done

if [ "$failed" -ne 0 ]; then
    echo "Some tests failed."
    exit 1
fi

echo "All ${#tests[@]} test program(s) passed."
