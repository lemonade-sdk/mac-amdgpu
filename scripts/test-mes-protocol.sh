#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
import re
source = Path('dext/amdgpu/mes_v12_1.cpp').read_text()
start = source.index('static uint32_t\nmes_ring_write(')
end = source.index('// mes_query_sched_status —', start)
submission = source[start:end]
diag_start = submission.index('static void\nmes_log_queue_state(')
diag_end = submission.index('//------------------------------------------------------------------\n// mes_submit_pkt', diag_start)
submission = submission[:diag_start] + submission[diag_end:]
Path('build/tests/mes_submission_under_test.inc').write_text(submission)
alloc_start = source.index('static kern_return_t\nmes_alloc_vram_block(')
alloc_end = source.index('//------------------------------------------------------------------\n// mes_alloc_storage', alloc_start)
Path('build/tests/mes_allocation_under_test.inc').write_text(source[alloc_start:alloc_end])
registers = Path('dext/amdgpu/amdgpu_mes_registers.h').read_text()
checks = []
for name in re.findall(r'constexpr Register (\w+)', registers):
    linux_name = name.removesuffix('_OFFSET')
    checks.append(f'static_assert(MESRegs::{name}.offset == reg{linux_name});')
    checks.append(f'static_assert(MESRegs::{name}.baseIndex == reg{linux_name}_BASE_IDX);')
Path('build/tests/mes_register_checks.inc').write_text('\n'.join(checks))
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I dext/amdgpu -I build/tests tests/mes_protocol_test.cpp \
  -o build/tests/mes-protocol-test
build/tests/mes-protocol-test
