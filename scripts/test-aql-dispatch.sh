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
Path('build/tests/aql_context.inc').write_text(h[h.index('struct AQLLaunch {'):h.index('kern_return_t aql_launch')]+h[h.index('struct PersistentAQLQueue'):h.index('kern_return_t aql_queue_open')])
# Verify the constants against the version-specific Linux branch, not its default.
linux=Path('upstream/linux/drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c').read_text()
sw=linux[linux.index('static int gfx_v12_0_sw_init'):]
branch=sw[sw.index('case IP_VERSION(12, 0, 1):'):sw.index('default:')]
pipes=int(re.search(r'num_pipe_per_mec\s*=\s*(\d+)',branch)[1])
queues=int(re.search(r'num_queue_per_pipe\s*=\s*(\d+)',branch[branch.index('adev->gfx.mec.num_mec'):])[1])
Path('build/tests/aql_topology.inc').write_text(f'static_assert(kGFX1201ComputePipes=={pipes});\nstatic_assert(kGFX1201QueuesPerPipe=={queues});\n')
s=Path('dext/amdgpu/amdgpu_aql.cpp').read_text()
Path('build/tests/aql_launch.inc').write_text(s[s.index('kern_return_t aql_launch'):s.rindex('\n}')])
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-sanitize-recover=all -I dext/amdgpu -I build/tests \
  tests/aql_dispatch_test.cpp -o build/tests/aql-dispatch-test
build/tests/aql-dispatch-test
