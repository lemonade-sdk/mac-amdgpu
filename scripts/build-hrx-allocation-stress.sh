#!/usr/bin/env bash
# Build only; no GPU operations.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/amdgpu-llvm-env.sh
hrx_lib="$PWD/build/hrx-macos-adapter/libhrx/src/libhrx"
"$llvm_bin/clang" -std=c11 -Wall -Wextra -Werror \
  -Ibuild/hrx-macos-source/libhrx/include hsa/tools/mac_hrx_allocation_stress.c \
  -L"$hrx_lib" -lhrx -Wl,-rpath,"$hrx_lib" \
  -o build/hrx-macos-adapter/mac-hrx-allocation-stress
printf 'Built allocation stress; explicit hardware command:\n'
printf 'DYLD_LIBRARY_PATH=%q %q --run\n' "$PWD/build/hsa" "$PWD/build/hrx-macos-adapter/mac-hrx-allocation-stress"
