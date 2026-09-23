#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/amdgpu-llvm-env.sh
mkdir -p build/tests
"$llvm_bin/clang" -target amdgcn-amd-amdhsa -mcpu=gfx1201 -nogpulib \
  -x cl -cl-std=CL2.0 -O2 -c tests/shaders/hsa_resources_gfx1201.cl \
  -o build/tests/hsa-resource-object.o
"${AMDGPU_LLD:-/opt/homebrew/bin/ld.lld}" -shared --no-undefined \
  build/tests/hsa-resource-object.o -o build/tests/hsa-resource-object.hsaco
"$llvm_bin/llvm-readelf" --notes build/tests/hsa-resource-object.hsaco
"$llvm_bin/llvm-objdump" -d build/tests/hsa-resource-object.hsaco > build/tests/hsa-resource-object.disasm
python3 - <<'PY'
from pathlib import Path
assembly = Path('build/tests/hsa-resource-object.disasm').read_text()
kernel = assembly.split('<scratch_lds>:', 1)[1].split('<__clang_ocl_kern_imp_', 1)[0]
assert 'scratch_store_b32' in kernel and 'scratch_load_b32' in kernel
store = kernel.index('ds_store_b32')
load = kernel.index('ds_load_b32', store)
assert 's_wait_dscnt 0x0' in kernel[store:load], 'LDS publication wait missing between cross-lane store/load'
print('Resource shader retains scratch accesses and an LDS publication wait.')
PY
