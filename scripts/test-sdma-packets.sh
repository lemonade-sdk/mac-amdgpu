#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
source = Path('dext/amdgpu/sdma_v7_0.cpp').read_text()
helper_start = source.index('kern_return_t\nsdma_clear_fence(')
helper_end = source.index('// sdma_ring_test —', helper_start)
Path('build/tests/sdma_wb_under_test.inc').write_text(source[helper_start:helper_end])
alloc_start = source.index('kern_return_t\nsdma_alloc_storage(')
alloc_end = source.index('// sdma_engine_halt —', alloc_start)
Path('build/tests/sdma_allocation_under_test.inc').write_text(source[alloc_start:alloc_end])
start = source.index('kern_return_t\nsdma_ring_test(')
end = source.index('// sdma_init_full —', start)
Path('build/tests/sdma_copy_under_test.inc').write_text(source[start:end])
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I dext/amdgpu -I build/tests tests/sdma_packet_test.cpp \
  -o build/tests/sdma-packet-test
build/tests/sdma-packet-test
