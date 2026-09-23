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
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I build/tests tests/shutdown_lifecycle_test.cpp -o build/tests/shutdown-lifecycle-test
build/tests/shutdown-lifecycle-test
