#!/usr/bin/env bash
# No HSA initialization or GPU work: compile the patched helper with fake calls.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
source=Path('build/hrx-macos-source/runtime/src/iree/hal/drivers/amdgpu/access_policy.c').read_text()
start=source.index('iree_status_t iree_hal_amdgpu_access_allow_agent_list(')
end=source.index('iree_status_t iree_hal_amdgpu_access_lock_host_allocation(',start)
Path('build/tests/hrx_access_policy_function.inc').write_text(source[start:end])
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -Wno-missing-field-initializers -UNDEBUG \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -Ihsa/third_party/hsa/include -Ibuild/tests \
  tests/hrx_access_policy_test.cpp -o build/tests/hrx-access-policy-test
build/tests/hrx-access-policy-test
