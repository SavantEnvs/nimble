# syntax=docker/dockerfile:1.7
#
# Commit-image Dockerfile (SMALL form, PREFERRED) — builds ON TOP of the org C/C++ base
# image (ghcr.io/savantenvs/base). The base already provides debian + clang/llvm +
# libFuzzer/afl++ + the identities + the build-contract ENV, so this only adds the
# per-commit build (built on top of the prebuilt base, rather than building the toolchain in-image).
#
# Build (from the repo root):
#   DOCKER_BUILDKIT=1 docker build -f mayhem/Dockerfile \
#       --build-arg COMMIT=$(git rev-parse HEAD) \
#       [--build-arg SOURCE_DATE_EPOCH=$(date +%s)] \
#       -t mytag .

# The org base, pinned to a fixed DATE-tagged base (YYYYMMDD; shared by all integrations
# minted against it). Use the current dated tag; pin by digest in production: ...base@sha256:<digest>.
FROM ghcr.io/savantenvs/base:20261023

ARG COMMIT
ARG REPO_URL
# SOURCE_DATE_EPOCH stays an ARG (available to build.sh); do NOT bake it as an ENV — an
# empty value breaks clang ("must be a non-negative decimal integer") both here and for
# any in-image rebuild. build.sh unsets it when empty.
ARG SOURCE_DATE_EPOCH

# Sanitizer override: the base exports SANITIZER_FLAGS (ASan+UBSan, halting) as the default.
# To change it for a build (e.g. ASan-only, or add MSan): --build-arg SANITIZER_FLAGS="...".
# Declare it WITHOUT a default (no '='): an undefaulted ARG lets the base ENV flow through to
# build.sh when not passed, and an explicit --build-arg overrides it. (An empty default — `=` —
# would instead shadow the base ENV and silently wipe the sanitizers.)
ARG SANITIZER_FLAGS
# Coverage (optional, off by default): instrument the TEST build to measure how much of the project
# the suite exercises — --build-arg COVERAGE_FLAGS="-fprofile-instr-generate -fcoverage-mapping".
# Empty by default (no effect on the normal oracle build); build.sh appends it to the test build only.
ARG COVERAGE_FLAGS=
# Build parallelism (optional). build.sh falls back to nproc when unset: --build-arg MAYHEM_JOBS=8
ARG MAYHEM_JOBS=

# ---- apt dependencies -------------------------------------------------------------------------
# Nimble builds Velox (the `velox` submodule) as part of its own CMake project; Velox's minimal+DWIO
# configuration resolves these from the system (CMake find_package), the same packages Velox's
# scripts/setup-ubuntu.sh installs. Debian trixie ships every one at a version Velox accepts except
# folly, simdjson (needs 4.1.0) and xsimd (needs 10.x) - those three are built below. (googletest
# is deliberately not installed: Nimble's and Velox's CMake build their own pinned copy, see below.)
USER root
RUN [ -n "$SOURCE_DATE_EPOCH" ] && repro-sources-list.sh || true ; \
    apt-get update && apt-get install -yq --no-install-recommends \
        bison flex libfl-dev \
        flatbuffers-compiler libflatbuffers-dev \
        protobuf-compiler libprotobuf-dev \
        libboost-atomic-dev libboost-chrono-dev libboost-context-dev libboost-date-time-dev \
        libboost-filesystem-dev libboost-program-options-dev libboost-regex-dev \
        libboost-system-dev libboost-thread-dev libboost-dev \
        libgflags-dev libgoogle-glog-dev libunwind-dev libfmt-dev libdouble-conversion-dev \
        libevent-dev libssl-dev libicu-dev libre2-dev libabsl-dev libfast-float-dev \
        libstemmer-dev libxxhash-dev libsodium-dev libelf-dev libdwarf-dev \
        liblz4-dev libzstd-dev libsnappy-dev liblzo2-dev zlib1g-dev libbz2-dev liblzma-dev \
 && rm -rf /var/lib/apt/lists/*

# ---- pinned source dependencies (SPEC 6.2 item 19) --------------------------------------------
# Built once, uninstrumented, static, out of tree from $d/src with -g -gdwarf-3, installed into
# /opt/toolchains/deps/<name>-<version>/{include,lib}. build.sh points CMAKE_PREFIX_PATH at them, so
# Velox's dependency resolution finds them as SYSTEM packages and never reaches for the network.
# Versions + sha256 are the ones Velox pins in velox/CMake/resolve_dependency_modules/*.cmake.
#
# DEP_ARCH_FLAGS: Velox and Nimble compile with the flags velox/scripts/setup-helper-functions.sh
# get_cxx_flags emits for CPU_TARGET=avx, and folly must be compiled with the same ones (folly's F14
# hash tables pick their SIMD mode from them; a mismatch fails at link time).
ARG DEP_ARCH_FLAGS="-mavx2 -mfma -mavx -mf16c -mlzcnt -mbmi2"

ARG XSIMD_VER=10.0.0
ARG XSIMD_SHA256=73f818368b3a4dad92fab1b2933d93694241bd2365a6181747b2df1768f6afdd
RUN d=/opt/toolchains/deps/xsimd-${XSIMD_VER} && mkdir -p "$d/src" /tmp/dep-build \
 && curl -fsSL -o /tmp/dep.tgz \
      https://github.com/xtensor-stack/xsimd/archive/refs/tags/${XSIMD_VER}.tar.gz \
 && echo "${XSIMD_SHA256}  /tmp/dep.tgz" | sha256sum -c - \
 && tar -xzf /tmp/dep.tgz --strip-components=1 -C "$d/src" \
 && cmake -S "$d/src" -B /tmp/dep-build -GNinja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$d" -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_TESTS=OFF \
 && cmake --build /tmp/dep-build --target install \
 && cd / && rm -rf /tmp/dep-build /tmp/dep.tgz && chmod -R a+rX "$d"

ARG SIMDJSON_VER=4.1.0
ARG SIMDJSON_SHA256=78115e37b2e88ec63e6ae20bb148063a9112c55bcd71404c8572078fd8a6ac3e
RUN d=/opt/toolchains/deps/simdjson-${SIMDJSON_VER} && mkdir -p "$d/src" /tmp/dep-build \
 && curl -fsSL -o /tmp/dep.tgz \
      https://github.com/simdjson/simdjson/archive/refs/tags/v${SIMDJSON_VER}.tar.gz \
 && echo "${SIMDJSON_SHA256}  /tmp/dep.tgz" | sha256sum -c - \
 && tar -xzf /tmp/dep.tgz --strip-components=1 -C "$d/src" \
 && cmake -S "$d/src" -B /tmp/dep-build -GNinja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$d" -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DSIMDJSON_DEVELOPER_MODE=OFF \
      -DCMAKE_CXX_FLAGS="-g -gdwarf-3" \
 && cmake --build /tmp/dep-build -j"${MAYHEM_JOBS:-$(nproc)}" --target install \
 && cd / && rm -rf /tmp/dep-build /tmp/dep.tgz && chmod -R a+rX "$d"

ARG FOLLY_VER=v2026.01.05.00
ARG FOLLY_SHA256=8b41494b664fcde3d02f652d6a27381ec5e938ef58cf986d8ce4958d424af101
RUN d=/opt/toolchains/deps/folly-${FOLLY_VER} && mkdir -p "$d/src" /tmp/dep-build \
 && curl -fsSL -o /tmp/dep.tgz \
      https://github.com/facebook/folly/releases/download/${FOLLY_VER}/folly-${FOLLY_VER}.tar.gz \
 && echo "${FOLLY_SHA256}  /tmp/dep.tgz" | sha256sum -c - \
 && tar -xzf /tmp/dep.tgz -C "$d/src" \
 && cmake -S "$d/src" -B /tmp/dep-build -GNinja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$d" -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_TESTS=OFF -DFOLLY_HAVE_INT128_T=ON \
      -DCMAKE_CXX_STANDARD=20 \
      -DCMAKE_C_FLAGS="-g -gdwarf-3" -DCMAKE_CXX_FLAGS="${DEP_ARCH_FLAGS} -g -gdwarf-3" \
 && cmake --build /tmp/dep-build -j"${MAYHEM_JOBS:-$(nproc)}" --target install \
 && cd / && rm -rf /tmp/dep-build /tmp/dep.tgz && chmod -R a+rX "$d"

# fsst: dwio/nimble/encodings/CMakeLists.txt fetches it with FetchContent (git, pinned to this
# commit) and compiles it into nimble_encodings. Source only: build.sh hands it to that same
# FetchContent call through FETCHCONTENT_SOURCE_DIR_FSST, so Nimble's build compiles it, offline.
ARG FSST_VER=50dd308befd5121bd1121428a833f42cce6e1f9d
ARG FSST_SHA256=3b14f636acbcfca7092e42664bf0a7139907c526963b1e227a6d6659e2bbedc1
RUN d=/opt/toolchains/deps/fsst-${FSST_VER} && mkdir -p "$d/src" \
 && curl -fsSL -o /tmp/dep.tgz https://github.com/cwida/fsst/archive/${FSST_VER}.tar.gz \
 && echo "${FSST_SHA256}  /tmp/dep.tgz" | sha256sum -c - \
 && tar -xzf /tmp/dep.tgz --strip-components=1 -C "$d/src" \
 && rm -rf /tmp/dep.tgz "$d"/src/*.mp4 "$d"/src/*.pptx "$d"/src/*.pdf "$d"/src/paper \
 && chmod -R a+rX "$d"
# Boost.Algorithm: Nimble's top-level CMakeLists.txt lists `algorithm` among the Boost COMPONENTS it
# requires. It is a header-only library, so Debian's libboost*-dev (like any Boost install whose
# CMake files were not generated for it) has no boost_algorithm package and find_package(Boost)
# fails. This adds exactly that missing package description for the installed Boost version: an
# interface target on top of Boost::headers, which is what Boost generates for header-only
# libraries. No Boost code is added or replaced.
RUN v="$(sed -n 's/^set(PACKAGE_VERSION \(.*\))$/\1/p' \
          /usr/lib/x86_64-linux-gnu/cmake/boost_headers-*/boost_headers-config-version.cmake)" \
 && [ -n "$v" ] \
 && d="/opt/toolchains/deps/boost-algorithm-cmake-${v}/lib/cmake/boost_algorithm-${v}" \
 && mkdir -p "$d" \
 && printf '%s\n' \
      'if(NOT TARGET Boost::algorithm)' \
      "  find_package(boost_headers ${v} EXACT CONFIG REQUIRED)" \
      '  add_library(Boost::algorithm INTERFACE IMPORTED)' \
      '  set_target_properties(Boost::algorithm PROPERTIES INTERFACE_LINK_LIBRARIES Boost::headers)' \
      'endif()' > "$d/boost_algorithm-config.cmake" \
 && cp "/usr/lib/x86_64-linux-gnu/cmake/boost_headers-${v}/boost_headers-config-version.cmake" \
       "$d/boost_algorithm-config-version.cmake" \
 && chmod -R a+rX "/opt/toolchains/deps/boost-algorithm-cmake-${v}"

# googletest: Nimble's top-level CMakeLists.txt asks for the package as lower-case `gtest`, which no
# system install provides, so upstream's build always takes Velox's bundled route (FetchContent of
# this exact release, velox/CMake/resolve_dependency_modules/gtest.cmake). Source only, like fsst:
# build.sh passes it as FETCHCONTENT_SOURCE_DIR_GOOGLETEST and the test tree compiles it.
ARG GTEST_VER=1.13.0
ARG GTEST_SHA256=ad7fdba11ea011c1d925b3289cf4af2c66a352e18d4c7264392fead75e919363
RUN d=/opt/toolchains/deps/googletest-${GTEST_VER} && mkdir -p "$d/src" \
 && curl -fsSL -o /tmp/dep.tgz \
      https://github.com/google/googletest/archive/refs/tags/v${GTEST_VER}.tar.gz \
 && echo "${GTEST_SHA256}  /tmp/dep.tgz" | sha256sum -c - \
 && tar -xzf /tmp/dep.tgz --strip-components=1 -C "$d/src" \
 && rm -f /tmp/dep.tgz && chmod -R a+rX "$d"
USER mayhem

# Source enters from the build CONTEXT (the CI checkout): the FULL repo (incl. .git
# history) is copied to /mayhem as `mayhem`; build.sh builds in place.
USER mayhem
WORKDIR /mayhem
COPY --chown=2000:2000 . /mayhem

# Anything a Dockerfile step materializes into the tree (downloaded fixtures, generated headers,
# vendored deps) MUST be committed or (re)created by build.sh: rlenv's patch grader runs
# `git clean -ffdX` before every graded build, so a gitignored, untracked file is gone by then and
# every honest patch scores zero (SPEC §6.2 item 16; verify-repo.sh checks it).
#
# OPTIONAL compile-cache pre-fill (SPEC §6.2 item 18). Bases minted with /opt/mayhem/ccache/prefill
# run the build under rlenv's pinned ccache, so the patch grader's first (otherwise cold) rebuild is
# warm — this is what keeps large C/C++ targets inside the build time limit. Older bases fall back to
# the plain build; a missing or stale pre-fill only costs a cold cache. Never enable ccache in
# build.sh or via ENV/PATH, and keep the compiler resolving by name (CC=clang) or nothing is cached.
RUN if [ -x /opt/mayhem/ccache/prefill ]; then /opt/mayhem/ccache/prefill bash mayhem/build.sh; \
    else bash mayhem/build.sh; fi

# Run the project's functional tests at build time IF the repo ships them (mayhem/test.sh is
# optional — omit it only when the repo has no tests meaningful for RL). A failing suite fails the
# image build, so the commit image is guaranteed to build AND pass its own tests.
RUN if [ -f mayhem/test.sh ]; then bash mayhem/test.sh; else echo "no mayhem/test.sh — repo ships no functional tests"; fi

LABEL org.opencontainers.image.source="${REPO_URL}" \
      org.opencontainers.image.revision="${COMMIT}" \
      rlenv.tier="commit" \
      rlenv.source_date_epoch="${SOURCE_DATE_EPOCH}" \
      rlenv.user="mayhem"

USER mayhem
WORKDIR /mayhem
