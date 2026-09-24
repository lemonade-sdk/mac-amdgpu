#!/usr/bin/env bash
# Compile only: this never initializes HSA or opens a GPU.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/amdgpu-llvm-env.sh
lse_source="$PWD/build/lse-macos-source"
lse_build="$PWD/build/lse-macos-adapter"
loom_dir="$PWD/build/hrx-macos-adapter/loom/binding/c"
"$llvm_bin/clang++" -std=c++26 -Wall -Wextra -Werror \
  -I"$lse_source/include" -Ihsa/src hsa/tools/mac_lse_loom_compile.cpp hsa/src/code_object.cpp \
  "$lse_build/src/backends/hrx/liblse_backend_hrx.a" \
  "$lse_build/liblse_core.a" -Wl,-force_load,"$lse_build/liblse_graph.a" "$lse_build/liblse_opt.a" \
  "$lse_build/src/backends/liblse_backend_registry.a" "$lse_build/liblse_ir.a" \
  -Wl,-force_load,"$lse_build/liblse_kernels.a" \
  -L"$loom_dir" -lloomc -Wl,-rpath,"$loom_dir" \
  -L"$llvm_bin/../lib/c++" -Wl,-rpath,"$llvm_bin/../lib/c++" \
  -o "$lse_build/mac-lse-loom-compile"
"$lse_build/mac-lse-loom-compile" build/tests/lse-loom-matmul.hsaco
"$llvm_bin/llvm-readelf" --notes build/tests/lse-loom-matmul.hsaco
