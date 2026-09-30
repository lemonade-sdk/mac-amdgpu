#!/usr/bin/env python3
"""Compile the borrowed K/V address contract and reject invalid storage bounds."""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, default=ROOT / 'build/hrx-macos-adapter')
args = parser.parse_args()
compiler = args.build / 'loom/src/loom/tools/loom-compile/loom-compile'
optimizer = args.build / 'loom/src/loom/tools/loom-opt/loom-opt'
source = (ROOT / 'tests/shaders/kv_fragment_addressing.loom').read_text()
with tempfile.TemporaryDirectory(prefix='kv-addressing-') as temporary:
    path = Path(temporary) / 'input.loom'
    binary = Path(temporary) / 'output.vmfb'
    path.write_text(source)
    subprocess.run([str(compiler), '--backend=amdgpu-hal', '--target=gfx1201',
                    f'--output={binary}', str(path)], check=True)
    low = subprocess.check_output([str(optimizer), '--pipeline=source-low', str(path)], text=True)
    assert '%bounded = low.slice %index[0]' in low, 'bounded index retained its wide register representation'
    assert 'amdgpu.global_load_b32>' in low, 'borrowed pointer did not use a full global address'
    for label, invalid in [
        ('empty storage', source.replace('byte_length = 262144', 'byte_length = 0')),
        ('unaligned contract', source.replace('base_alignment = 16', 'base_alignment = 3')),
    ]:
        path.write_text(invalid)
        result = subprocess.run([str(optimizer), '--pipeline=source-low', str(path)],
                                text=True, capture_output=True)
        if result.returncode == 0 or 'error' not in result.stderr:
            raise RuntimeError(f'{label} was not rejected by the address contract: {result.stderr}')
print('PASS K/V borrowed addresses, narrowed indices, and invalid storage bounds')
