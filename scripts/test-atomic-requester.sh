#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
s=Path('dext/MacAMDGPU.cpp').read_text()
a=s.index('    case kMacAMDGPUMethodAtomicRequesterExperiment:')
b=s.index('    case kMacAMDGPUMethodCollectMetrics:',a)
Path('build/tests/atomic_requester_rpc.inc').write_text(s[a:b])
assert 'kMacAMDGPUMethodAtomicRequesterExperiment = 60' in s
assert 'mac_amdgpu_quiesce_for_shutdown(ivars->retainedPCI,phase,&ivars->atomicRequester)' in s
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I dext/amdgpu -I build/tests tests/atomic_requester_test.cpp -o build/tests/atomic-requester-test
build/tests/atomic-requester-test
