#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
s = Path('dext/amdgpu/gmc_v12_0.cpp').read_text()
parts = []
for signature in ['static void\nhub_init_gart_aperture_regs(', 'kern_return_t\ngmc_program_gart_window(']:
    start = s.index(signature)
    parts.append(s[start:s.index('\n}', start) + 2])
Path('build/tests/gmc_window_under_test.inc').write_text('\n'.join(parts))
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I dext/amdgpu -I build/tests tests/gmc_window_test.cpp -o build/tests/gmc-window-test
build/tests/gmc-window-test
