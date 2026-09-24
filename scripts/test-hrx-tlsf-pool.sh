#!/usr/bin/env bash
# Exercise the production TLSF pool with CPU slabs; no HSA/GPU initialization.
# Optional argument: an isolated tlsf_pool.c source to qualify before promotion.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/amdgpu-llvm-env.sh
python3 - "$llvm_bin" "$@" <<'PY'
from pathlib import Path
import json
import shlex
import subprocess
import sys

root = Path.cwd()
llvm = Path(sys.argv[1])
source = root / 'build/hrx-macos-source'
build = root / 'build/hrx-macos-adapter'
out = root / 'build/tests/hrx-tlsf-pool'
out.mkdir(parents=True, exist_ok=True)
if len(sys.argv) > 3:
    raise SystemExit('usage: scripts/test-hrx-tlsf-pool.sh [tlsf_pool.c]')
pool_source = (Path(sys.argv[2]).resolve() if len(sys.argv) == 3 else
               source / 'runtime/src/iree/hal/memory/tlsf_pool.c')
commands = json.loads((build / 'compile_commands.json').read_text())
command = next(item['command'] for item in commands
               if item['file'].endswith('/hal/memory/tlsf_pool.c'))
args = shlex.split(command)
obj = out / 'tlsf_pool.c.o'
args[args.index('-o') + 1] = str(obj)
args[args.index('-c') + 1] = str(pool_source)
subprocess.run(args, cwd=build, check=True)

# Reuse the configured runtime's exact dependency archives. Substitute only
# the pool object, so baseline and isolated source use identical dependencies.
commands = subprocess.check_output(
    ['ninja', '-C', str(build), '-t', 'commands', 'hrx'], text=True).splitlines()
link = next(shlex.split(line) for line in reversed(commands)
            if '-dynamiclib' in line)
archives = []
for word in link:
    if not word.endswith('.a'):
        continue
    path = Path(word)
    path = path if path.is_absolute() else build / path
    if path.name == 'libiree_hal_memory_tlsf_pool.a':
        continue
    archives.append(str(path))
exe = out / 'hrx-tlsf-pool-test'
args = [str(llvm / 'clang++'), '-std=c++20', '-O2', '-Wall', '-Wextra',
        '-Werror', '-Wno-missing-field-initializers', '-DIREE_ALLOCATOR_SYSTEM_CTL=iree_allocator_libc_ctl',
        '-isystem', str(source / 'runtime/src'),
        '-isystem', str(build / 'runtime/src'),
        str(root / 'tests/hrx_tlsf_pool_test.cpp'), str(obj), *archives,
        '-framework', 'CoreFoundation', '-o', str(exe)]
subprocess.run(args, check=True)
print(f'TLSF pool source: {pool_source}', flush=True)
raise SystemExit(subprocess.call([str(exe)]))
PY
