#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
s=Path('dext/amdgpu/amdgpu_regs.h').read_text()
parts=[]
for name in ('poll_psp_response','poll_reg'):
    a=s.index('\n'+name+'('); a=s.rfind('static inline',0,a); b=s.index('\n}',a)+2; parts.append(s[a:b])
s=Path('dext/amdgpu/smu_v14_0.cpp').read_text()
a=s.index('static bool\nsmu_wait_for_response'); b=s.index('\nkern_return_t\nsmu_send_msg(',a)
parts.append(s[a:b])
Path('build/tests/firmware_waits_under_test.inc').write_text('\n'.join(parts))
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I dext/amdgpu -I build/tests tests/firmware_waits_test.cpp -o build/tests/firmware-waits-test
build/tests/firmware-waits-test
