#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/amdgpu-llvm-env.sh
mkdir -p build/tests
"$llvm_bin/clang" -target amdgcn-amd-amdhsa -mcpu=gfx1201 -nogpulib \
  -x cl -cl-std=CL2.0 -O2 -c hsa/src/signal_operations.cl -o build/tests/signal-operations.o
"${AMDGPU_LLD:-/opt/homebrew/bin/ld.lld}" -shared --no-undefined \
  build/tests/signal-operations.o -o build/tests/signal-operations.hsaco
python3 - <<'PY'
from pathlib import Path
blob=Path('build/tests/signal-operations.hsaco').read_bytes()
lines=['#pragma once','#include <stdint.h>','namespace mac_hsa {',
       '// Compiled from signal_operations.cl by scripts/build-signal-operations.sh.',
       'inline constexpr uint8_t kSignalOperationsCodeObject[] = {']
lines += ['    '+','.join(f'0x{byte:02x}' for byte in blob[i:i+24])+',' for i in range(0,len(blob),24)]
lines+=['};','}']
Path('hsa/src/signal_operations_code.h').write_text('\n'.join(lines)+'\n')
PY
