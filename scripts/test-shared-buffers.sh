#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
s = Path('dext/MacAMDGPU.cpp').read_text()
a=s.index('static inline uint64_t\nmac_amdgpu_bo_make_handle(')
b=s.index('//\n// CS handle helpers',a)
Path('build/tests/bo_table_under_test.inc').write_text(s[a:b])
assert 'BOEntry  *boPages[amdgpu::kMaxBOPages];' in s
assert 'const auto tableStatus = mac_amdgpu_bo_find_free_slot(ivars, idx);' in s
assert 'if (!entry) return kIOReturnNotReady;' in s[s.index('// v0.1.27 — per-BO mapping'): ]
a=s.index('    case kMacAMDGPUMethodBOAlloc:')
b=s.index('    case kMacAMDGPUMethodBOExport:',a)
Path('build/tests/bo_alloc_under_test.inc').write_text(s[a:b])
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
