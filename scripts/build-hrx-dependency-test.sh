#!/usr/bin/env bash
# Build only. Hardware execution requires explicit --run, serially.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ $# -eq 0 || $# -eq 2 ]] || {
  echo 'usage: scripts/build-hrx-dependency-test.sh [patched-HRX-source HRX-build]' >&2
  exit 2
}
hrx_source=${1:-"$PWD/build/hrx-macos-source"}
hrx_build=${2:-"$PWD/build/hrx-macos-adapter"}
hrx_source=$(cd "$hrx_source" && pwd)
hrx_build=$(cd "$hrx_build" && pwd)
hrx_lib="$hrx_build/libhrx/src/libhrx"
[[ -f build/tests/hrx_compute_fixture.h ]] || {
  echo 'First run scripts/build-hrx-compute-fixture.sh (offline shader build).' >&2
  exit 1
}
cc=${CC:-/usr/bin/clang}
"$cc" -std=c11 -O2 -Wall -Wextra -Werror \
  -I "$hrx_source/libhrx/include" -I "$PWD/build/tests" \
  hsa/tools/mac_hrx_dependency_test.c -L "$hrx_lib" -lhrx \
  -Wl,-rpath,"$hrx_lib" -o "$hrx_build/mac-hrx-dependency-test"
# No-argument invocation only prints usage; it never initializes the runtime.
"$hrx_build/mac-hrx-dependency-test"
