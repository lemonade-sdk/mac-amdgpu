#!/usr/bin/env python3
"""Validate server dialect parsing without backend initialization or sockets."""
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
server = Path(sys.argv[1]) if len(sys.argv) > 1 else root / 'build/lse-macos-adapter/lse-server'
env = dict(os.environ)
env.pop('LSE_MODEL', None)
cases = [
    (['--help'], 0, '--dialect NAME'),
    (['--dialect', 'loom', '--help'], 0, 'Endpoints:'),
    (['--dialect', 'hip', '--help'], 0, 'Endpoints:'),
    (['--dialect', 'unknown', '--model', '/not-opened'], 2, "no dialect is spelled 'unknown'"),
    (['--dialect'], 2, '--dialect needs a value'),
    (['--dialect', 'loom'], 2, 'no model.'),
]
for args, code, message in cases:
    result = subprocess.run([str(server), *args], env=env, capture_output=True, text=True, timeout=10)
    output = result.stdout + result.stderr
    if result.returncode != code or message not in output:
        raise SystemExit(f'FAIL {args!r}: exit={result.returncode}\n{output}')
print(f'PASS {len(cases)} server CLI cases; no backend, model, or HTTP server opened')
