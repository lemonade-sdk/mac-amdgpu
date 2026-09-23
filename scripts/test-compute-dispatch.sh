#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
llvm_bin="${AMDGPU_LLVM_BIN:-/opt/homebrew/opt/llvm/bin}"
"$llvm_bin/llvm-mc" -triple=amdgcn-amd-amdhsa -mcpu=gfx1201 -mattr=+wavefrontsize32 \
  -filetype=obj tests/shaders/dispatch_gfx1201.s -o build/tests/dispatch.o
"$llvm_bin/llvm-objcopy" --dump-section .text=build/tests/dispatch.bin build/tests/dispatch.o
python3 - <<'PY'
from pathlib import Path
import re, struct
s = Path('Host/MacAMDGPUHostApp.swift').read_text()
words = re.search(r'let dispatchCode: \[UInt32\] = \[([^]]+)\]', s).group(1)
values = [int(w, 16) for w in re.findall('0x[0-9a-f]+', words)]
assert Path('build/tests/dispatch.bin').read_bytes() == struct.pack('<'+'I'*len(values), *values)
h = Path('dext/amdgpu/amdgpu_dispatch.h').read_text()
Path('build/tests/dispatch_context.inc').write_text(h[h.index('struct ComputeLaunch {'):h.index('kern_return_t compute_launch')])
s = Path('dext/amdgpu/amdgpu_dispatch.cpp').read_text()
Path('build/tests/dispatch_launch.inc').write_text(s[s.index('kern_return_t compute_launch'):s.rindex('\n}')])
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-sanitize-recover=all -I dext/amdgpu -I build/tests \
  tests/compute_dispatch_test.cpp -o build/tests/compute-dispatch-test
build/tests/compute-dispatch-test
