#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
llvm_bin="${AMDGPU_LLVM_BIN:-/opt/homebrew/opt/llvm/bin}"
"$llvm_bin/llvm-mc" -triple=amdgcn-amd-amdhsa -mcpu=gfx1201 \
  -mattr=+wavefrontsize32 -filetype=obj tests/shaders/compute_smoke_gfx1201.s \
  -o build/tests/compute-smoke.o
"$llvm_bin/llvm-objcopy" --dump-section .text=build/tests/compute-smoke.bin build/tests/compute-smoke.o
python3 - <<'PY'
from pathlib import Path
import re, struct
h = Path('dext/amdgpu/amdgpu_compute_packets.h').read_text()
code = h.split('kComputeSmokeCode[] = {', 1)[1].split('};', 1)[0]
words = [int(x, 16) for x in re.findall(r'0x[0-9a-f]+', code)]
assert Path('build/tests/compute-smoke.bin').read_bytes() == struct.pack('<' + 'I' * len(words), *words)
h = Path('dext/amdgpu/amdgpu_compute_test.h').read_text()
Path('build/tests/compute_context.inc').write_text(h[h.index('struct ComputeTest {'):h.index('kern_return_t compute_test')])
s = Path('dext/amdgpu/amdgpu_compute_test.cpp').read_text()
Path('build/tests/compute_test.inc').write_text(s[s.index('static uint32_t'):s.rindex('\n}')])
print('gfx1201 shader matches assembled bytes')
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-sanitize-recover=all -I dext/amdgpu -I build/tests \
  tests/compute_smoke_test.cpp -o build/tests/compute-smoke-test
build/tests/compute-smoke-test
