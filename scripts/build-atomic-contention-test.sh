#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/amdgpu-llvm-env.sh
mkdir -p build/tests
"$llvm_bin/clang" -target amdgcn-amd-amdhsa -mcpu=gfx1201 -nogpulib \
  -x cl -cl-std=CL2.0 -O2 -c tests/shaders/atomic_contention_gfx1201.cl \
  -o build/tests/hsa-atomic-contention.o
"${AMDGPU_LLD:-/opt/homebrew/bin/ld.lld}" -shared --no-undefined \
  build/tests/hsa-atomic-contention.o -o build/tests/hsa-atomic-contention.hsaco
"$llvm_bin/llvm-objdump" -d build/tests/hsa-atomic-contention.hsaco > build/tests/hsa-atomic-contention.disasm
"$llvm_bin/llvm-readelf" --notes build/tests/hsa-atomic-contention.hsaco
# Catch an accidental lowering to a non-atomic or 32-bit instruction.
rg -q 'global_atomic_add_(u64|b64).*scope:SCOPE_SYS' build/tests/hsa-atomic-contention.disasm
rg -q 'global_atomic_cmpswap_b64.*scope:SCOPE_SYS' build/tests/hsa-atomic-contention.disasm
printf 'Built native gfx1201 64-bit add/CAS fixture; no GPU work submitted.\n'
