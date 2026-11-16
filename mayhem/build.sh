#!/usr/bin/env bash
#
# mayhem/build.sh - build Nimble's fuzz targets and its test suite.
#
# Runs inside the commit image (mayhem/Dockerfile) as `mayhem` in /mayhem, and is re-run offline
# by the PATCH tier after `git clean -ffdX`. The base image exports the build contract:
#   CC, CXX             clang / clang++ (reached by name through PATH)
#   LIB_FUZZING_ENGINE  -fsanitize=fuzzer
#   SANITIZER_FLAGS     ASan + UBSan, halting
#   STANDALONE_FUZZ_MAIN  LLVM's run-once driver (C source)
#   SRC                 /mayhem
#
# Nimble is built by its own CMake project, unchanged: mayhem/CMakeLists.txt adds the repo's
# top-level CMakeLists.txt as a subdirectory and only appends the harness targets. That project
# builds the `velox` and `openzl` submodules from the checkout as part of the same build. What the
# upstream build would otherwise download (folly, simdjson, xsimd, fsst, googletest) is pre-built / pre-fetched
# under /opt/toolchains/deps by the Dockerfile and found from there, so nothing here touches the
# network.
#
# Two build trees, both under _build/ (gitignored upstream, so `git clean -ffdX` removes them):
#   _build/mayhem-fuzz   everything from the checkout compiled with $SANITIZER_FLAGS $DEBUG_FLAGS;
#                        produces /mayhem/fuzz_* and /mayhem/fuzz_*-standalone
#   _build/mayhem-tests  upstream's Release configuration, normal flags; produces the gtest
#                        binaries mayhem/test.sh runs (and mayhem_gen_seeds)
set -euo pipefail

# clang rejects SOURCE_DATE_EPOCH='' (empty) - it must be unset or a valid integer.
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

# SANITIZER_FLAGS uses `=` (not `:=`): an explicit EMPTY value builds with no sanitizers.
: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${STANDALONE_FUZZ_MAIN:=/opt/mayhem/StandaloneFuzzTargetMain.c}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
: "${SRC:=/mayhem}"
export CC CXX

# UBSan's `alignment` check is relaxed, and only that one. Nimble's decoders (and Velox's) read
# and write multi-byte integers at arbitrary byte offsets of a stream through casted pointers on
# purpose - the format is packed and the code targets x86-64/aarch64, where that is well defined in
# practice. The check fires on the first value of nearly every valid stream, so with halting
# sanitizers no input would get past it. ASan and every other UBSan check stay on and halting.
FUZZ_SANITIZER_FLAGS="$SANITIZER_FLAGS"
case "$SANITIZER_FLAGS" in
  *undefined*) FUZZ_SANITIZER_FLAGS="$SANITIZER_FLAGS -fno-sanitize=alignment" ;;
esac

# Coverage feedback for Nimble's own libraries (mayhem/CMakeLists.txt applies it to dwio/ only).
FUZZ_COVERAGE_FLAGS=""
case "$LIB_FUZZING_ENGINE" in
  *-fsanitize=fuzzer*) FUZZ_COVERAGE_FLAGS="-fsanitize=fuzzer-no-link" ;;
esac

cd "$SRC"

# ---- dependencies cached in the image (SPEC 6.2 item 19) -------------------------------------
DEPS=/opt/toolchains/deps
PREFIX_PATH=""
for d in "$DEPS"/*/; do
  [ -d "${d}lib" ] || [ -d "${d}include" ] || continue
  PREFIX_PATH="${PREFIX_PATH:+$PREFIX_PATH;}${d%/}"
done
FSST_SRC="$(ls -d "$DEPS"/fsst-*/src 2>/dev/null | head -1)"
GTEST_SRC="$(ls -d "$DEPS"/googletest-*/src 2>/dev/null | head -1)"
[ -n "$PREFIX_PATH" ] && [ -n "$FSST_SRC" ] && [ -n "$GTEST_SRC" ] || {
  echo "build.sh: $DEPS is missing the cached dependencies (folly/simdjson/xsimd/fsst/googletest)" >&2
  exit 1
}

# Velox's flag helper autodetects the CPU from /proc/cpuinfo unless CPU_TARGET is set; pin it to
# what upstream's Makefile uses (`CPU_TARGET ?= "avx"`) so the build does not depend on the host
# and matches the flags folly was compiled with.
export CPU_TARGET=avx

# Options shared by both trees. Upstream's Makefile passes CMAKE_BUILD_TYPE=Release and
# VELOX_ENABLE_GEO=OFF; the rest keeps dependency resolution inside the image:
#   VELOX_DEPENDENCY_SOURCE=SYSTEM  find_package only - a missing dependency is a configure error,
#                                   never a download
#   gtest_SOURCE=BUNDLED            the one exception, and what upstream's build does too: the
#                                   top-level CMakeLists.txt asks for lower-case `gtest`, which only
#                                   Velox's bundled googletest satisfies
#   FETCHCONTENT_*                  fsst and googletest come from the pre-fetched sources
#   CMAKE_FIND_PACKAGE_TARGETS_GLOBAL  Nimble's libraries link protobuf::libprotobuf, which Velox's
#                                   CMakeLists.txt looks up; with a system protobuf that imported
#                                   target would otherwise be visible inside velox/ only
#   CMAKE_CXX_SCAN_FOR_MODULES=OFF  nothing here uses C++20 modules; without this, CMake >= 3.28
#                                   runs clang-scan-deps over every C++20 source
#   VELOX_ENABLE_CCACHE=OFF         no compiler launcher: the compiler is reached by name, and the
#                                   grader supplies its own cache
COMMON_ARGS=(
  -GNinja
  -DCMAKE_BUILD_TYPE=Release
  -DVELOX_ENABLE_GEO=OFF
  -DVELOX_ENABLE_CCACHE=OFF
  -DVELOX_DEPENDENCY_SOURCE=SYSTEM
  -DCMAKE_FIND_PACKAGE_TARGETS_GLOBAL=ON
  -DCMAKE_CXX_SCAN_FOR_MODULES=OFF
  "-DCMAKE_PREFIX_PATH=$PREFIX_PATH"
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON
  "-DFETCHCONTENT_SOURCE_DIR_FSST=$FSST_SRC"
  -Dgtest_SOURCE=BUNDLED
  "-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=$GTEST_SRC"
  -Wno-dev
)

# mayhem/lsan_off.cc (__lsan_is_turned_off() -> 1) is compiled with the sanitizer flags and linked
# into every fuzz and -standalone binary: leak detection off, ASan/UBSan on (SPEC 6.2 item 15).
[ -f "$SRC/mayhem/lsan_off.cc" ] || { echo "build.sh: mayhem/lsan_off.cc is missing" >&2; exit 1; }

# ---- 1) fuzz tree: sanitized project + harnesses + standalone reproducers ---------------------
# Release semantics (NDEBUG), like the build that ships: debug-only checks stay out of the way so
# a bad read reaches the sanitizer instead of being turned into an exception first. -O1 keeps
# backtraces readable and the instrumented build affordable.
FUZZERS=(fuzz_velox_reader fuzz_selective_reader fuzz_encoding fuzz_encoding_legacy fuzz_deserializer)
FUZZ_TARGETS=()
for f in "${FUZZERS[@]}"; do FUZZ_TARGETS+=("$f" "$f-standalone"); done

cmake -S mayhem -B _build/mayhem-fuzz "${COMMON_ARGS[@]}" \
  -DMAYHEM_BUILD_FUZZERS=ON \
  -DCMAKE_C_FLAGS="$FUZZ_SANITIZER_FLAGS $DEBUG_FLAGS" \
  -DCMAKE_CXX_FLAGS="$FUZZ_SANITIZER_FLAGS $DEBUG_FLAGS" \
  -DCMAKE_C_FLAGS_RELEASE="-O1 -DNDEBUG" \
  -DCMAKE_CXX_FLAGS_RELEASE="-O1 -DNDEBUG" \
  -DMAYHEM_COVERAGE_FLAGS="$FUZZ_COVERAGE_FLAGS" \
  -DMAYHEM_FUZZ_ENGINE="$LIB_FUZZING_ENGINE" \
  -DMAYHEM_STANDALONE_MAIN="$STANDALONE_FUZZ_MAIN" \
  -DMAYHEM_LSAN_OFF="$SRC/mayhem/lsan_off.cc"
cmake --build _build/mayhem-fuzz -j"$MAYHEM_JOBS" --target "${FUZZ_TARGETS[@]}"
# Publish them at the repo root (/mayhem/<fuzzer>, /mayhem/<fuzzer>-standalone). Hard links, not
# copies: each binary is statically linked against the sanitized Velox and carries full debug info.
for t in "${FUZZ_TARGETS[@]}"; do
  ln -f "_build/mayhem-fuzz/bin/$t" "$SRC/$t" 2>/dev/null \
    || install -m 0755 "_build/mayhem-fuzz/bin/$t" "$SRC/$t"
done

# ---- 2) test tree: upstream's own suite, normal flags -----------------------------------------
# Every test Nimble registers with CTest (add_test) is a gtest binary named after its test; build
# exactly those, plus the seed generator. $COVERAGE_FLAGS is empty unless a coverage build asks.
cmake -S mayhem -B _build/mayhem-tests "${COMMON_ARGS[@]}" \
  -DMAYHEM_BUILD_SEEDGEN=ON \
  -DCMAKE_C_FLAGS="$COVERAGE_FLAGS" \
  -DCMAKE_CXX_FLAGS="$COVERAGE_FLAGS"
# A subset of upstream's suites: the ones covering the code the harnesses fuzz (common, encodings,
# serializer/deserializer, tablet). mayhem/test-suites.txt is the list test.sh runs too; each suite
# is a separate binary statically linking Velox, so building all 14 would double the build time.
mapfile -t TEST_TARGETS < <(grep -v '^#' mayhem/test-suites.txt | sed '/^$/d')
[ "${#TEST_TARGETS[@]}" -gt 0 ] || { echo "build.sh: mayhem/test-suites.txt lists no tests" >&2; exit 1; }
cmake --build _build/mayhem-tests -j"$MAYHEM_JOBS" --target "${TEST_TARGETS[@]}" mayhem_gen_seeds

echo "build.sh: built ${#FUZZERS[@]} fuzz targets (+standalone) and ${#TEST_TARGETS[@]} test binaries"
