#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/amdgpu-llvm-env.sh
mkdir -p build/tests
"$llvm_bin/clang" -target amdgcn-amd-amdhsa -mcpu=gfx1201 -nogpulib -x cl -cl-std=CL2.0 -O2 -c hsa/src/signal_mailbox_service.cl -o build/tests/signal-mailbox-service.o
"$llvm_bin/clang" -target amdgcn-amd-amdhsa -mcpu=gfx1201 -nogpulib -x cl -cl-std=CL2.0 -O2 -S -emit-llvm hsa/src/signal_mailbox_service.cl -o build/tests/signal-mailbox-service.ll
"${AMDGPU_LLD:-/opt/homebrew/bin/ld.lld}" -shared --no-undefined build/tests/signal-mailbox-service.o -o build/tests/signal-mailbox-service.hsaco
"$llvm_bin/llvm-readelf" --notes build/tests/signal-mailbox-service.hsaco > build/tests/signal-mailbox-service.metadata
"$llvm_bin/llvm-objdump" -d build/tests/signal-mailbox-service.hsaco > build/tests/signal-mailbox-service.disasm
python3 - <<'PY'
from pathlib import Path
ir=Path('build/tests/signal-mailbox-service.ll').read_text()
isa=Path('build/tests/signal-mailbox-service.disasm').read_text()
metadata=Path('build/tests/signal-mailbox-service.metadata').read_text()
assert 'syncscope(' not in ir
for operation in ('xchg','add','sub','and','or','xor'):
    assert 'atomicrmw '+operation in ir,operation
assert 'cmpxchg' in ir and 'seq_cst seq_cst' in ir
assert ' acquire,' in ir and ' release,' in ir
assert 'global_inv scope:SCOPE_SYS' in isa and 'global_wb scope:SCOPE_SYS' in isa
assert '.group_segment_fixed_size: 0' in metadata and '.private_segment_fixed_size: 0' in metadata
assert '.kernarg_segment_size: 32' in metadata and '.wavefront_size: 32' in metadata
blob=Path('build/tests/signal-mailbox-service.hsaco').read_bytes()
lines=['#pragma once','#include <stdint.h>','namespace mac_hsa {',
       '// Compiled from signal_mailbox_service.cl by scripts/build-signal-mailbox-service.sh.',
       'inline constexpr uint8_t kSignalMailboxServiceCode[] = {']
lines += ['    '+','.join(f'0x{byte:02x}' for byte in blob[i:i+24])+',' for i in range(0,len(blob),24)]
lines+=['};','}']
Path('hsa/src/signal_mailbox_service_code.h').write_text('\n'.join(lines)+'\n')
PY
