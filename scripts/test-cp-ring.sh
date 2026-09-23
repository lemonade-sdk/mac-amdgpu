#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
import re
s = Path('dext/amdgpu/cp_v12_0.cpp').read_text()
functions = []
for name in ['cp_alloc_storage', 'cp_release_storage', 'cp_read_fence', 'cp_read_cs_fence', 'cp_read_rptr', 'cp_configure_rs64', 'cp_ring_write', 'cp_emit_eop_fence', 'cp_map_gfx_queue', 'cp_enable', 'cp_compute_enable', 'cp_kick_doorbell', 'cp_submit_eop_test', 'cp_scratch_test', 'cp_kiq_smoke_test']:
    a = s.index('\n' + name + '(')
    a = s.rfind('\n', 0, a) + 1
    b = s.index('\n}', a) + 2
    functions.append(s[a:b])
Path('build/tests/cp_under_test.inc').write_text('\n'.join(functions))
h = Path('dext/amdgpu/amdgpu_cp.h').read_text()
a = h.index('constexpr uint32_t kCPRingDefaultBytes')
b = h.index('\n};', a) + 3
Path('build/tests/cp_context_under_test.inc').write_text(h[a:b].replace('#ifdef __APPLE__', '').replace('#endif', ''))
checks = []
for ns in ['CP', 'GFX']:
    h = Path(f'dext/amdgpu/amdgpu_{ns.lower()}_registers.h').read_text()
    for n in re.findall(r'constexpr Register (\w+)', h):
        checks += [f'static_assert({ns}Regs::{n}.offset == reg{n});',
                   f'static_assert({ns}Regs::{n}.baseIndex == reg{n}_BASE_IDX);']
Path('build/tests/cp_register_checks.inc').write_text('\n'.join(checks))
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I dext/amdgpu -I build/tests tests/cp_ring_test.cpp -o build/tests/cp-ring-test
build/tests/cp-ring-test
