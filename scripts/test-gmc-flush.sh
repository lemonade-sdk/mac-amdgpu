#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
s=Path('dext/amdgpu/gmc_v12_0.cpp').read_text(); a=s.index('kern_return_t\ngmc_flush_gpu_tlb('); b=s.index('\n}',a)+2
Path('build/tests/gmc_flush_under_test.inc').write_text(s[a:b])
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I dext/amdgpu -I build/tests tests/gmc_flush_test.cpp -o build/tests/gmc-flush-test
build/tests/gmc-flush-test
