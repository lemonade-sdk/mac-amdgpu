#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
s = Path('dext/MacAMDGPU.cpp').read_text()
a = s.index('    case kMacAMDGPUMethodCollectMetrics:')
b = s.index('    case kMacAMDGPUMethodRuntimeBuild:', a)
Path('build/tests/metrics_rpc_under_test.inc').write_text(s[a:b])
a = s.index('    if (driver->ivars->shutdownBlocked &&')
b = s.index('    kern_return_t admission =', a)
Path('build/tests/metrics_allowlist_under_test.inc').write_text(s[a:b])
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I dext/amdgpu -I build/tests tests/metrics_rpc_test.cpp -o build/tests/metrics-rpc-test
build/tests/metrics-rpc-test
