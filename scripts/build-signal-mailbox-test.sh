#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/amdgpu-llvm-env.sh
mkdir -p build/tests
"$llvm_bin/clang" -target amdgcn-amd-amdhsa -mcpu=gfx1201 -nogpulib -x cl -cl-std=CL2.0 -O2 -c tests/shaders/signal_mailbox_gfx1201.cl -o build/tests/hsa-signal-mailbox.o
"$llvm_bin/clang" -target amdgcn-amd-amdhsa -mcpu=gfx1201 -nogpulib -x cl -cl-std=CL2.0 -O2 -S -emit-llvm tests/shaders/signal_mailbox_gfx1201.cl -o build/tests/hsa-signal-mailbox.ll
"${AMDGPU_LLD:-/opt/homebrew/bin/ld.lld}" -shared --no-undefined build/tests/hsa-signal-mailbox.o -o build/tests/hsa-signal-mailbox.hsaco
"$llvm_bin/llvm-objdump" -d build/tests/hsa-signal-mailbox.hsaco > build/tests/hsa-signal-mailbox.disasm
"$llvm_bin/llvm-readelf" --notes build/tests/hsa-signal-mailbox.hsaco > build/tests/hsa-signal-mailbox.metadata
python3 - <<'PY'
from pathlib import Path
ir=Path('build/tests/hsa-signal-mailbox.ll').read_text()
isa=Path('build/tests/hsa-signal-mailbox.disasm').read_text()
assert 'syncscope(' not in ir
for operation in ('xchg','add','sub','and','or','xor'):
    assert 'atomicrmw '+operation in ir,operation
assert 'cmpxchg' in ir and 'seq_cst seq_cst' in ir
assert ' acquire,' in ir and ' release,' in ir
assert 'global_atomic_' in isa and 'scope:SCOPE_SYS' in isa
assert 'global_inv scope:SCOPE_SYS' in isa and 'global_wb scope:SCOPE_SYS' in isa
PY
printf 'Built gfx1201 signal mailbox fixture (no GPU execution).\n'
