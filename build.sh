#!/usr/bin/env bash
# Copyright (c) 2026 Furkan Selim Cetin <contact@furkancetin.dev>
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Developed at the German Cancer Research Center (DKFZ).

set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${project_dir}/build"
build_type=Release
run_tests=OFF
jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-}"
if [[ -z "$jobs" ]]; then
  jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2')"
  if (( jobs > 4 )); then jobs=4; fi
fi

usage() {
  cat <<'EOF'
Usage: ./build.sh [options]

Configure and compile the Jetson Control desktop GUI. Run from any directory.
Requires Qt 6.5+ development packages, CMake 3.21+, and a C++20 compiler.
Ninja is used for a new build directory when available.

Options:
  --test             Also build and run the test suite.
  --debug            Build Debug instead of Release.
  --build-dir DIR    Build directory (default: build/ beside this script).
                    Relative paths are resolved from the current directory.
  --jobs N, -j N     Parallel jobs (default: up to 4 CPU cores, or
                    CMAKE_BUILD_PARALLEL_LEVEL when set).
  --help, -h         Show this help.

For a custom Qt installation, set CMAKE_PREFIX_PATH to its installation prefix.
EOF
}

die() { printf 'build.sh: %s\n' "$*" >&2; exit 2; }

while (( $# )); do
  case "$1" in
    --test) run_tests=ON; shift ;;
    --debug) build_type=Debug; shift ;;
    --build-dir)
      [[ $# -ge 2 && -n "$2" ]] || die '--build-dir requires a directory.'
      build_dir="$2"; shift 2 ;;
    --jobs|-j)
      [[ $# -ge 2 ]] || die "$1 requires a positive integer."
      jobs="$2"; shift 2 ;;
    --help|-h) usage; exit 0 ;;
    *) die "Unknown option: $1 (use --help)." ;;
  esac
done

[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || die 'Parallel jobs must be a positive integer.'
command -v cmake >/dev/null 2>&1 || die 'CMake was not found. Install CMake 3.21 or newer.'
if [[ "$run_tests" == ON ]]; then
  command -v ctest >/dev/null 2>&1 || die 'CTest was not found. Install the CMake test tools.'
fi

configure_args=(-S "$project_dir" -B "$build_dir"
  "-DCMAKE_BUILD_TYPE=$build_type" "-DBUILD_TESTING=$run_tests")
if [[ ! -f "$build_dir/CMakeCache.txt" ]] && command -v ninja >/dev/null 2>&1; then
  configure_args+=(-G Ninja)
fi

cmake "${configure_args[@]}"
if [[ "$run_tests" == ON ]]; then
  cmake --build "$build_dir" --config "$build_type" --parallel "$jobs"
  ctest --test-dir "$build_dir" --build-config "$build_type" --output-on-failure --parallel "$jobs"
else
  cmake --build "$build_dir" --config "$build_type" --parallel "$jobs" --target jetson-control
fi

printf '\nBuild complete. Executable directory: %s\n' "$(cd -- "$build_dir" && pwd)"
