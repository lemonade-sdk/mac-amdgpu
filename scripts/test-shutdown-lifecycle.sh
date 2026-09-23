#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
source = Path('dext/MacAMDGPU.cpp').read_text()
start = source.index('static kern_return_t\nmac_amdgpu_quiesce_for_shutdown(')
end = source.index('//============================================================\n// MacAMDGPUUserClient::Stop', start)
Path('build/tests/shutdown_under_test.inc').write_text(source[start:end])
start = source.index('void\nMacAMDGPUUserClient::FinishStop(')
end = source.index('//============================================================', start)
Path('build/tests/finish_stop_under_test.inc').write_text(source[start:end])
start = source.index('static void\nmac_amdgpu_release_quarantine(')
end = source.index('// Keep all DMA backing pinned', start)
Path('build/tests/release_quarantine_under_test.inc').write_text(source[start:end])
start = source.index('static bool\nmac_amdgpu_retire_client_queues(')
end = source.index('static void\nmac_amdgpu_release_quarantine(', start)
Path('build/tests/retire_client_under_test.inc').write_text(source[start:end])
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I build/tests tests/shutdown_lifecycle_test.cpp -o build/tests/shutdown-lifecycle-test
build/tests/shutdown-lifecycle-test
