#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
h = Path('dext/amdgpu/amdgpu_gart.h').read_text()
a = h.index('struct GARTBinding {')
b = h.index('\n};', h.index('struct GARTContext {')) + 3
Path('build/tests/gart_context_under_test.inc').write_text(
    h[a:b].replace('#ifdef __APPLE__', '').replace('#endif', ''))
s = Path('dext/amdgpu/amdgpu_gart.cpp').read_text()
functions = []
for name in ['gart_bus_range', 'gart_table_range', 'gart_invalidate', 'gart_init',
             'gart_configure_host_window', 'gart_bind_range', 'gart_bind_existing', 'gart_unbind', 'gart_bind_sysmem', 'gart_release_after_reset']:
    # Helpers place their return type/name on one line.
    import re
    match = re.search(r'^(?:static (?:bool|kern_return_t) )?' + name + r'\(', s, re.M)
    a = match.start()
    if s[a:a+len(name)] == name:
        a = s.rfind('\n', 0, a-1) + 1
    b = s.index('\n}', a) + 2
    functions.append(s[a:b])
s = Path('dext/amdgpu/gmc_v12_0.cpp').read_text()
a = s.index('kern_return_t\ngmc_bind_existing(DeviceContext')
b = s.index('\n}', a) + 2
functions.append(s[a:b])
Path('build/tests/gart_binding_under_test.inc').write_text('\n'.join(functions))
h = Path('dext/amdgpu/amdgpu_memory_test.h').read_text()
a = h.index('struct MemoryTransferTest {')
b = h.index('\n};', h.index('struct MemoryTransferResult {')) + 3
Path('build/tests/memory_test_context.inc').write_text(h[a:b])
s = Path('dext/amdgpu/amdgpu_memory_test.cpp').read_text()
Path('build/tests/memory_transfer_under_test.inc').write_text(
    s[s.index('static constexpr'):s.rindex('\n}')])
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I dext/amdgpu -I build/tests tests/gart_binding_test.cpp -o build/tests/gart-binding-test
build/tests/gart-binding-test

xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I dext/amdgpu tests/gart_allocator_test.cpp -o build/tests/gart-allocator-test
build/tests/gart-allocator-test
