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
python3 - <<'PY'
from pathlib import Path
text=Path('build/tests/hsa-atomic-contention.disasm').read_text()
contention=text.split('<atomic_contention>:',1)[1].split('<__clang_ocl_kern_imp_',1)[0]
adds=[line for line in contention.splitlines() if 'global_atomic_add_u64' in line]
assert any('TH_ATOMIC_RETURN' in line and 'scope:SCOPE_SYS' in line for line in adds)
assert any('TH_ATOMIC_RETURN' not in line and 'scope:SCOPE_SYS' in line for line in adds)
handoff=text.split('<atomic_handoff>:',1)[1].split('<__clang_ocl_kern_imp_',1)[0]
for opcode in ('global_atomic_add_u64','global_atomic_swap_b64','global_atomic_cmpswap_b64'):
    assert any(opcode in line and 'TH_ATOMIC_RETURN' in line and 'scope:SCOPE_SYS' in line
               for line in handoff.splitlines()), opcode
PY
printf 'Built native gfx1201 add/no-return, add/return, CAS and serialized handoff fixture; no GPU work submitted.\n'
