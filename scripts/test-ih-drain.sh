#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
s=Path('dext/amdgpu/ih_v7_0.cpp').read_text(); a=s.index('uint32_t\nih_drain('); b=s.index('\n}',a)+2
Path('build/tests/ih_drain_under_test.inc').write_text(s[a:b])
h=Path('dext/amdgpu/amdgpu_ih.h').read_text(); a=h.index('constexpr uint32_t kIHRingDefaultBytes'); b=h.index('//\n// API',a)
Path('build/tests/ih_types_under_test.inc').write_text(h[a:b])
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I dext/amdgpu -I build/tests tests/ih_drain_test.cpp -o build/tests/ih-drain-test
build/tests/ih-drain-test
