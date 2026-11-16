#!/usr/bin/env bash
#
# mayhem/test.sh — RUN Nimble's OWN functional test suite (already built by mayhem/build.sh).
# exit 0 = pass. PATCH-grade oracle: after an agent patches the source, the grader rebuilds
# (build.sh) then runs this.
#
# IMPORTANT:
#  * Must assert BEHAVIOR/OUTPUT, not just exit status. The oracle has to check asserted values /
#    golden-output diffs / known-answer results — so a PATCH that "fixes" a bug by making the program
#    exit(0) (or any no-op) FAILS here. Running inputs and checking only "exit 0 / didn't crash" is
#    NOT a functional test (it's trivially reward-hackable) — use the project's real assertion suite.
#  * Do NOT build here — mayhem/build.sh already compiled the test suite (with the project's normal
#    flags). This script only RUNS the pre-built tests and reports counts. If the test runner is
#    missing, that's a build.sh bug — fail loudly rather than silently rebuilding.
#  * REQUIRED OUTPUT — a CTRF (https://ctrf.io) summary so Mayhem/the PATCH grader reads the counts:
#      - writes a CTRF JSON report to ${CTRF_REPORT:-$SRC/ctrf-report.json}, and
#      - prints a one-line `CTRF {...}` marker to stdout (same JSON, compact).
#    Only `results.summary` (with tests/passed/failed/pending/skipped/other) is required.
#    Use the emit_ctrf helper below; it computes tests = passed+failed+skipped and sets the exit
#    code (0 iff failed==0). Map your framework's output to passed/failed/skipped.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"   # build parallelism; env-overridable, falls back to nproc (use -j"$MAYHEM_JOBS")
cd "$SRC"

# emit_ctrf <tool> <passed> <failed> [skipped] [pending] [other]
# Writes a CTRF report (file + stdout `CTRF {...}` marker) and returns non-zero iff failed>0.
emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

# Nimble's suite is the set of gtest binaries its CMake registers with CTest (add_test); build.sh
# built them, with upstream's normal Release flags, in _build/mayhem-tests. CTest only looks at each
# binary's exit status, so instead of `ctest` each binary is run with the exact command line CTest
# would use plus --gtest_output=xml, and the counts come from the reports the binaries write: a
# binary that produces no report (crashed, or did nothing) counts as a failure.
BUILD="$SRC/_build/mayhem-tests"
[ -f "$BUILD/CTestTestfile.cmake" ] || {
  echo "test.sh: $BUILD is missing - mayhem/build.sh must build the test suite first" >&2
  emit_ctrf "gtest" 0 1 0
  exit 1
}

REPORTS="$(mktemp -d "${TMPDIR:-/tmp}/nimble-tests.XXXXXX")"
trap 'rm -rf "$REPORTS"' EXIT

# One "name<TAB>cwd<TAB>arg0<TAB>arg1..." line per registered test.
(cd "$BUILD" && ctest --show-only=json-v1) | python3 -c '
import json, sys
wanted = {l.strip() for l in open(sys.argv[1]) if l.strip() and not l.startswith("#")}
for t in json.load(sys.stdin)["tests"]:
    cwd = next((p["value"] for p in t.get("properties", []) if p["name"] == "WORKING_DIRECTORY"), ".")
    if t["name"] in wanted:
        print("\t".join([t["name"], cwd] + t["command"]))
' "$SRC/mayhem/test-suites.txt" > "$REPORTS/tests.tsv"
[ -s "$REPORTS/tests.tsv" ] || {
  echo "test.sh: CTest lists no tests in $BUILD" >&2
  emit_ctrf "gtest" 0 1 0
  exit 1
}

# Tests that already fail on pristine upstream code in this build (the test tree compiles upstream
# unmodified, so a failure there is a property of upstream at this commit, not of a patch). They are
# listed, with the reason, in mayhem/test-excludes.txt and passed to every binary as a negative
# --gtest_filter; everything else in the suites runs.
EXCLUDES="$(grep -v '^#' "$SRC/mayhem/test-excludes.txt" 2>/dev/null | sed 's/[[:space:]]*#.*//; /^$/d' | paste -sd: -)"
FILTER=()
[ -n "$EXCLUDES" ] && FILTER=(--gtest_filter="-$EXCLUDES")

run_one() {
  local name="$1" cwd="$2"; shift 2
  if [ ! -x "$1" ]; then
    echo "test.sh: test binary $1 is missing - mayhem/build.sh should have built it" >&2
    return 1
  fi
  (cd "$cwd" && "$@" "${FILTER[@]}" --gtest_output="xml:$REPORTS/$name.xml") > "$REPORTS/$name.log" 2>&1
  local rc=$?
  echo "$rc" > "$REPORTS/$name.rc"
  [ "$rc" -eq 0 ] || { echo "---- $name exited $rc; last lines:"; tail -n 40 "$REPORTS/$name.log"; }
  return 0
}

# Up to $MAYHEM_JOBS binaries at a time.
while IFS=$'\t' read -r -a fields; do
  while [ "$(jobs -rp | wc -l)" -ge "$MAYHEM_JOBS" ]; do wait -n || true; done
  run_one "${fields[@]}" &
done < "$REPORTS/tests.tsv"
wait

# Sum the gtest reports. A test binary without a readable report, or with a non-zero exit status
# its report does not account for, adds one failure.
read -r passed failed skipped < <(python3 - "$REPORTS" <<'PY'
import os, sys
import xml.etree.ElementTree as ET
d = sys.argv[1]
passed = failed = skipped = 0
for line in open(os.path.join(d, "tests.tsv")):
    name = line.split("\t", 1)[0]
    try:
        rc = int(open(os.path.join(d, name + ".rc")).read().strip())
    except (OSError, ValueError):
        rc = 1
    try:
        root = ET.parse(os.path.join(d, name + ".xml")).getroot()
    except (OSError, ET.ParseError):
        print(f"test.sh: {name}: no gtest report (exit {rc})", file=sys.stderr)
        failed += 1
        continue
    for case in root.iter("testcase"):
        if case.find("failure") is not None or case.find("error") is not None:
            print(f"test.sh: FAILED {name}: {case.get('classname')}.{case.get('name')}", file=sys.stderr)
    tests = int(root.get("tests", 0))
    bad = int(root.get("failures", 0)) + int(root.get("errors", 0))
    skip = int(root.get("disabled", 0)) + int(root.get("skipped", 0))
    if rc != 0 and bad == 0:
        bad = 1
    failed += bad
    skipped += skip
    passed += max(tests - bad - skip, 0)
    print(f"test.sh: {name}: tests={tests} failed={bad} skipped={skip} exit={rc}", file=sys.stderr)
print(passed, failed, skipped)
PY
)

emit_ctrf "gtest" "${passed:-0}" "${failed:-1}" "${skipped:-0}"
