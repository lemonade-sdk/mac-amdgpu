#!/usr/bin/env bash
# Offline compilation only; embeds the code object into the HRX smoke tool.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/amdgpu-llvm-env.sh
mkdir -p build/tests
"$llvm_bin/clang" -target amdgcn-amd-amdhsa -mcpu=gfx1201 -nogpulib \
  -x cl -cl-std=CL2.0 -O2 -ffp-contract=off -c \
  tests/shaders/hrx_compute_gfx1201.cl -o build/tests/hrx-compute.o
"${AMDGPU_LLD:-/opt/homebrew/bin/ld.lld}" -shared --no-undefined \
  build/tests/hrx-compute.o -o build/tests/hrx-compute.hsaco
"$llvm_bin/llvm-readelf" --notes build/tests/hrx-compute.hsaco > build/tests/hrx-compute.metadata
"$llvm_bin/llvm-objdump" -d build/tests/hrx-compute.hsaco > build/tests/hrx-compute.disasm
python3 - <<'PY'
from pathlib import Path
data = Path('build/tests/hrx-compute.hsaco').read_bytes()
rows = [', '.join(f'0x{b:02x}' for b in data[i:i+16]) for i in range(0, len(data), 16)]
Path('build/tests/hrx_compute_fixture.h').write_text(
    'static const unsigned char hrx_compute_fixture[] = {\n' +
    ',\n'.join(rows) + '\n};\n')
PY
printf 'Built gfx1201 HRX vector/matrix compute fixture; no GPU work submitted.\n'
