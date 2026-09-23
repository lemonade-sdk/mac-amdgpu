#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
import re
h=Path('dext/amdgpu/amdgpu_aql_packets.h').read_text()
namespace=h[h.index('namespace AQLMQDOff {'):h.index('// DWORD doorbell')]
fields=re.findall(r'(\w+)\s*=\s*\d+',namespace)
Path('build/tests/aql_mqd_offsets.inc').write_text('\n'.join(f'static_assert(AQLMQDOff::{f}*4==offsetof(v12_compute_mqd,{f}));' for f in fields))
h=Path('dext/amdgpu/amdgpu_aql.h').read_text()
Path('build/tests/aql_context.inc').write_text(h[h.index('struct AQLLaunch {'):h.index('kern_return_t aql_launch')])
s=Path('dext/amdgpu/amdgpu_aql.cpp').read_text()
Path('build/tests/aql_launch.inc').write_text(s[s.index('kern_return_t aql_launch'):s.rindex('\n}')])
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-sanitize-recover=all -I dext/amdgpu -I build/tests \
  tests/aql_dispatch_test.cpp -o build/tests/aql-dispatch-test
build/tests/aql-dispatch-test
