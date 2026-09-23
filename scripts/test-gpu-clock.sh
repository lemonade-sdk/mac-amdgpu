#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
s = Path('dext/amdgpu/amdgpu_clock.cpp').read_text()
Path('build/tests/gpu_clock_reader_under_test.inc').write_text(s[s.index('namespace amdgpu {'):])
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I dext/amdgpu -I build/tests -I upstream/linux/drivers/gpu/drm/amd/include \
  -I upstream/linux/drivers/gpu/drm/amd/include/asic_reg \
  tests/gpu_clock_test.cpp -o build/tests/gpu-clock-test
build/tests/gpu-clock-test
