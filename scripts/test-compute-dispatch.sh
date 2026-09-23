#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
llvm_bin="${AMDGPU_LLVM_BIN:-/opt/homebrew/opt/llvm/bin}"
"$llvm_bin/llvm-mc" -triple=amdgcn-amd-amdhsa -mcpu=gfx1201 -mattr=+wavefrontsize32 \
  -filetype=obj tests/shaders/dispatch_gfx1201.s -o build/tests/dispatch.o
"$llvm_bin/llvm-objcopy" --dump-section .text=build/tests/dispatch.bin build/tests/dispatch.o
"$llvm_bin/clang" -target amdgcn-amd-amdhsa -mcpu=gfx1201 -nogpulib \
  -x cl -cl-std=CL2.0 -O2 -c tests/shaders/dispatch_gfx1201.cl -o build/tests/dispatch-oracle.o
"$llvm_bin/llvm-objcopy" --dump-section .text=build/tests/dispatch-oracle.bin build/tests/dispatch-oracle.o
"$llvm_bin/llvm-nm" --print-size --defined-only build/tests/dispatch-oracle.o > build/tests/dispatch-oracle.nm
python3 - <<'PY'
from pathlib import Path
import re, struct
symbols = Path('build/tests/dispatch-oracle.nm').read_text().splitlines()
entry = next(line.split() for line in symbols if line.split()[-1] == 'vector_add')
start, size = int(entry[0], 16), int(entry[1], 16)
oracle = Path('build/tests/dispatch-oracle.bin').read_bytes()[start:start+size]
assert oracle == Path('build/tests/dispatch.bin').read_bytes(), 'test shader differs from compiler output'
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
