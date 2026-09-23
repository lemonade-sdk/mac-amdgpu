#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
xcrun clang++ -std=c++20 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I dext/amdgpu -I upstream/linux/drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if \
  tests/metrics_test.cpp -o build/tests/metrics-test
build/tests/metrics-test
xcrun clang++ -std=c++20 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I dext/amdgpu tests/metrics_collection_test.cpp -o build/tests/metrics-collection-test
build/tests/metrics-collection-test
python3 - <<'PY'
from pathlib import Path
s = Path('dext/amdgpu/smu_v14_0.cpp').read_text()
a = s.index('kern_return_t\nsmu_smc_hw_setup(')
b = s.index('\n}', a) + 2
Path('build/tests/metrics_setup_under_test.inc').write_text(s[a:b])
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I dext/amdgpu -I build/tests tests/metrics_setup_test.cpp -o build/tests/metrics-setup-test
build/tests/metrics-setup-test
