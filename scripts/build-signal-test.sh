#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/amdgpu-llvm-env.sh
mkdir -p build/tests
"$llvm_bin/clang" -target amdgcn-amd-amdhsa -mcpu=gfx1201 -nogpulib \
  -x cl -cl-std=CL2.0 -O2 -c tests/shaders/signal_gfx1201.cl -o build/tests/hsa-signal-object.o
"${AMDGPU_LLD:-/opt/homebrew/bin/ld.lld}" -shared --no-undefined \
  build/tests/hsa-signal-object.o -o build/tests/hsa-signal-object.hsaco
"$llvm_bin/llvm-objdump" -d build/tests/hsa-signal-object.hsaco > build/tests/hsa-signal-object.disasm
