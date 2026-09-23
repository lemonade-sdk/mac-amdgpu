#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
import re
header = Path('dext/amdgpu/amdgpu_mes.h').read_text()
# Keep the real ABI/state declarations; replace only SDK-backed MMIO below.
header = re.sub(r'^#include.*\n', '', header, flags=re.M)
Path('build/tests/mes_header_under_test.inc').write_text(header)
source = Path('dext/amdgpu/mes_v12_1.cpp').read_text()
source = re.sub(r'^#include.*\n', '', source, flags=re.M)
source = re.sub(r'#define MES_LOG.*\\\n.*\n', '', source)
a = source.index('static uint32_t\nmes_ring_write(')
b = source.index('kern_return_t\nmes_set_hw_resources(', a)
source = source[:a] + source[b:]
Path('build/tests/mes_startup_under_test.inc').write_text(source)
checks=[]
for field in re.findall(r'constexpr uint32_t (\w+)\s*=', source[source.index('namespace MQDOff {'):source.index('static inline uint32_t order_base_2')]):
    checks.append(f'static_assert(MQDOff::{field} * 4 == offsetof(v12_compute_mqd, {field}));')
# Validate every locally defined field mask/shift against AMD's register header.
linux = Path('upstream/linux/drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_0_0_sh_mask.h').read_text()
for name in re.findall(r'^#define (\w+__(?:\w+))\s+0x', header, flags=re.M):
    m = re.search(r'^#define '+name+r'\s+(0x[0-9a-fA-F]+)', linux, re.M)
    assert m, name
    checks.append(f'static_assert({name} == {m[1]}u);')
Path('build/tests/mes_mqd_checks.inc').write_text('\n'.join(checks))
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I dext/amdgpu -I build/tests tests/mes_startup_test.cpp -o build/tests/mes-startup-test
build/tests/mes-startup-test
