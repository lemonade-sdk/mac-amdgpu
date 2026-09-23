#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
h = Path('dext/amdgpu/amdgpu_gfx.h').read_text()
end = h.index('//\n// Port of gfx_v12_0_constants_init')
Path('build/tests/gfx_config_under_test.inc').write_text(h[h.index('namespace amdgpu {'):end]+'\n}\n')
s = Path('dext/amdgpu/gfx_v12_0.cpp').read_text()
end = s.index('// Convenience overload')
Path('build/tests/gfx_constants_under_test.inc').write_text(s[s.index('namespace amdgpu {'):end]+'\n}\n')
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-sanitize-recover=all -I dext/amdgpu -I build/tests \
  tests/gfx_constants_test.cpp -o build/tests/gfx-constants-test
build/tests/gfx-constants-test
