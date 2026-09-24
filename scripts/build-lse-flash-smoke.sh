#!/usr/bin/env bash
# Build only: running the executable requires an explicit --run.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/amdgpu-llvm-env.sh
lse_source="$PWD/build/lse-macos-source"
lse_build="$PWD/build/lse-macos-adapter"
hrx_dir="$PWD/build/hrx-macos-adapter/libhrx/src/libhrx"
loom_dir="$PWD/build/hrx-macos-adapter/loom/binding/c"
"$llvm_bin/clang++" -std=c++26 -Wall -Wextra -Werror -I"$lse_source/include" \
  hsa/tools/mac_lse_flash_smoke.cpp \
  -Wl,-force_load,"$lse_build/src/backends/hrx/liblse_backend_hrx.a" \
  -Wl,-force_load,"$lse_build/liblse_place.a" \
  -Wl,-force_load,"$lse_build/liblse_kernels.a" \
  -Wl,-force_load,"$lse_build/liblse_graph.a" \
  "$lse_build/liblse_probe.a" "$lse_build/liblse_dist.a" "$lse_build/liblse_communication.a" \
  "$lse_build/liblse_opt.a" "$lse_build/liblse_ir.a" "$lse_build/liblse_trace.a" \
  "$lse_build/src/backends/liblse_backend_registry.a" "$lse_build/liblse_core.a" \
  -L"$hrx_dir" -lhrx -Wl,-rpath,"$hrx_dir" \
  -L"$loom_dir" -lloomc -Wl,-rpath,"$loom_dir" \
  -L"$llvm_bin/../lib/c++" -Wl,-rpath,"$llvm_bin/../lib/c++" \
  -o "$lse_build/mac-lse-flash-smoke"
printf 'Built LSE flash attention GPU smoke; no GPU calls run. Explicit command:\n'
printf 'DYLD_LIBRARY_PATH=%q %q --run\n' "$PWD/build/hsa" "$lse_build/mac-lse-flash-smoke"
