#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
import re
s = Path('dext/amdgpu/gmc_v12_0.cpp').read_text()
a = s.index('static void\nhub_init_cache_regs(')
b = s.index('\n}', a) + 2
Path('build/tests/gmc_cache_under_test.inc').write_text(s[a:b])
local = Path('dext/amdgpu/amdgpu_field_defs.h').read_text()
linux = Path('upstream/linux/drivers/gpu/drm/amd/include/asic_reg')
checks = []
for prefix, filename in [('MM', 'mmhub/mmhub_4_1_0_sh_mask.h'), ('GC', 'gc/gc_12_0_0_sh_mask.h')]:
    values = dict(re.findall(r'#define\s+(\w+)\s+(0x[0-9a-fA-F]+|\d+)[uUlL]*', (linux / filename).read_text()))
    for reg, field in sorted(set(re.findall(r'REG_SET_FIELD\([^,]+,\s*(\w+),\s*(\w+),', s))):
        if not reg.startswith(('MMMC_', 'MMVM_')):
            continue
        for suffix in ('MASK', '_SHIFT'):
            name = reg + '__' + field + '_' + suffix
            upstream = name if prefix == 'MM' else name.replace('MMMC_', 'GCMC_').replace('MMVM_', 'GCVM_')
            if upstream in values and re.search(r'#define\s+' + re.escape(name) + r'\s', local):
                checks.append(f'static_assert({name} == {values[upstream]}, "{upstream} differs from Linux");')
Path('build/tests/gmc_field_checks.inc').write_text('\n'.join(checks))
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I dext/amdgpu -I build/tests tests/gmc_cache_test.cpp -o build/tests/gmc-cache-test
build/tests/gmc-cache-test
