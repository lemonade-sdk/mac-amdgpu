#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/amdgpu-llvm-env.sh
mkdir -p build/tests
"$llvm_bin/clang" -target amdgcn-amd-amdhsa -mcpu=gfx1201 -nogpulib \
  -x cl -cl-std=CL2.0 -O2 -c tests/shaders/ownership_ping_pong_gfx1201.cl \
  -o build/tests/hsa-ownership-ping-pong.o
"$llvm_bin/clang" -target amdgcn-amd-amdhsa -mcpu=gfx1201 -nogpulib \
  -x cl -cl-std=CL2.0 -O2 -S -emit-llvm tests/shaders/ownership_ping_pong_gfx1201.cl \
  -o build/tests/hsa-ownership-ping-pong.ll
"${AMDGPU_LLD:-/opt/homebrew/bin/ld.lld}" -shared --no-undefined \
  build/tests/hsa-ownership-ping-pong.o -o build/tests/hsa-ownership-ping-pong.hsaco
"$llvm_bin/llvm-objdump" -d build/tests/hsa-ownership-ping-pong.hsaco > build/tests/hsa-ownership-ping-pong.disasm
"$llvm_bin/llvm-readelf" --notes build/tests/hsa-ownership-ping-pong.hsaco
python3 - <<'PY'
from pathlib import Path
import re
ir=Path('build/tests/hsa-ownership-ping-pong.ll').read_text()
isa=Path('build/tests/hsa-ownership-ping-pong.disasm').read_text()
# LLVM's omitted syncscope denotes system scope. The first kernel argument
# points at TURN; require acquire/release on that exact ownership word.
assert re.search(r'load atomic i64, ptr addrspace\(1\) %0 acquire',ir)
assert re.search(r'store atomic i64 %\w+, ptr addrspace\(1\) %0 release',ir)
assert 'syncscope(' not in ir
assert 'atomicrmw' not in ir and 'cmpxchg' not in ir
assert 'global_atomic_' not in isa
# Verify the emitted ownership operations, including cache/wait lowering.
# Ownership is offset zero from the data base, while payload/control cells have
# nonzero offsets or a separate derived base. Changed allocation of registers
# intentionally requires inspection and updating this fixture oracle.
lines=isa.splitlines()
loads=[i for i,line in enumerate(lines) if 'global_load_b64' in line and 's[4:5] scope:SCOPE_SYS' in line]
stores=[i for i,line in enumerate(lines) if 'global_store_b64' in line and 's[4:5] scope:SCOPE_SYS' in line]
assert loads and stores
for i in loads:
    assert 's_wait_loadcnt 0x0' in lines[i+1] and 'global_inv scope:SCOPE_SYS' in lines[i+2]
for i in stores:
    assert 'global_wb scope:SCOPE_SYS' in lines[i-2] and 's_wait_storecnt 0x0' in lines[i-1]
assert all('scope:SCOPE_SYS' in line for line in lines if 'global_load_b64' in line or 'global_store_b64' in line)
PY
printf 'Built separate gfx1201 ownership ping-pong fixture; existing atomic A/B fixture untouched.\n'
