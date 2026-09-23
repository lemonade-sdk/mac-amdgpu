#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'EXTRACT'
from pathlib import Path
s=Path('dext/MacAMDGPU.cpp').read_text()
a=s.index('    case kMacAMDGPUMethodBOCopy:')
b=s.index('    case kMacAMDGPUMethodBOAlloc:',a)
Path('build/tests/buffer_rpc_under_test.inc').write_text(s[a:b])
EXTRACT
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I dext/amdgpu -I build/tests tests/buffer_rpc_test.cpp -o build/tests/buffer-rpc-test
build/tests/buffer-rpc-test
