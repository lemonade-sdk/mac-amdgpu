#!/usr/bin/env python3
"""Test real Loom binary/assembly denorm policy without opening a GPU.

Uses an already built HRX tree's compile flags/generated headers/dependency
archives. Recompiles the selected source tree's descriptor writer and calls
its actual assembly metadata printer; does not alter that build or its libs.
"""
import argparse
import json
import shlex
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--hrx-build', type=Path, default=root/'build/hrx-macos-adapter')
p.add_argument('--hrx-source', type=Path, default=root/'build/hrx-macos-source')
p.add_argument('--output', type=Path, default=root/'build/tests/loom-denorm-descriptor')
a = p.parse_args()
base, source, out = (x.resolve() for x in (a.hrx_build, a.hrx_source, a.output))
out.mkdir(parents=True, exist_ok=True)
entries = json.loads((base/'compile_commands.json').read_text())
entry = next(e for e in entries if e['file'].endswith('/native/amdgpu/descriptor.c'))
relative = Path('loom/src/loom/target/emit/native/amdgpu/descriptor.c')
# Use the selected checked-in headers with the build's matching generated ones.
for label, src in [('descriptor', source/relative),
                   ('test', root/'tests/loom_denorm_descriptor_test.c')]:
    cmd = shlex.split(entry['command'])
    cmd[cmd.index('-c')+1] = str(src)
    cmd[cmd.index('-o')+1] = str(out/f'{label}.o')
    cmd[1:1] = [f'-I{source}/loom/src', f'-I{source}/runtime/src', f'-I{source}']
    subprocess.run(cmd, cwd=entry['directory'], check=True)
# Extract the existing compiler's complete dependency library list. No Ninja
# build is invoked and no library/output in that tree is changed.
raw = subprocess.check_output(
    ['ninja','-t','commands','loom/binding/c/libloomc.0.1.0.dylib'],
    cwd=base, text=True).splitlines()[-1]
link = shlex.split(raw.removeprefix(': && ').removesuffix(' && :'))
cmd = [link[0], '-Wl,-no_warn_duplicate_libraries', str(out/'test.o'),
       str(out/'descriptor.o')]
cmd += [x for x in link[1:] if x.endswith('.a')]
cmd += ['-o', str(out/'test')]
subprocess.run(cmd, cwd=base, check=True)
subprocess.run([str(out/'test')], check=True)
