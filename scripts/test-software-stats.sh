#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
s=Path('dext/MacAMDGPU.cpp').read_text()
a=s.index('static void mac_amdgpu_observe_software_queues(')
b=s.index('static void mac_amdgpu_raw_work_completed(',a)
Path('build/tests/software_queue_observer.inc').write_text(s[a:b])
a=s.index('    case kMacAMDGPUMethodSoftwareSnapshot:')
b=s.index('    case kMacAMDGPUMethodPing:',a)
Path('build/tests/software_snapshot_rpc.inc').write_text(s[a:b])
s=Path('dext/amdgpu/sdma_v7_0.cpp').read_text()
a=s.index('kern_return_t\nsdma_copy_linear_test(')
b=s.index('//------------------------------------------------------------------',a)
Path('build/tests/software_sdma_copy.inc').write_text(s[a:b])
PY
for name in software_stats software_stats_rpc software_sdma; do
  xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-sanitize-recover=all -I dext/amdgpu -I build/tests \
    "tests/${name}_test.cpp" -o "build/tests/${name}-test"
  "build/tests/${name}-test"
done
