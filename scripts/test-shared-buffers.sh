#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
s = Path('dext/MacAMDGPU.cpp').read_text()
a = s.index('    case kMacAMDGPUMethodBOExport:')
b = s.index('    case kMacAMDGPUMethodBOGetInfo:', a)
Path('build/tests/shared_buffer_rpc_under_test.inc').write_text(s[a:b])
a = s.index('static void\nmac_amdgpu_bo_release_all(', s.index('// Release every BO owned'))
b = s.index('static void\nmac_amdgpu_release_client_storage(', a)
Path('build/tests/shared_buffer_release_under_test.inc').write_text(s[a:b])
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I dext/amdgpu -I build/tests tests/shared_buffers_test.cpp -o build/tests/shared-buffers-test
build/tests/shared-buffers-test
