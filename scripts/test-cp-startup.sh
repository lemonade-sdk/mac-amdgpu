#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
cp = Path('dext/amdgpu/cp_v12_0.cpp').read_text()
parts = []
for name in ('cp_prepare_firmware', 'cp_start_engines', 'cp_init_full'):
    start = cp.index('\n' + name + '(')
    start = cp.rfind('\n', 0, start) + 1
    end = cp.index('\n}', start) + 2
    parts.append(cp[start:end])
Path('build/tests/cp_startup_under_test.inc').write_text('\n'.join(parts))
src = Path('dext/amdgpu/amdgpu_init.cpp').read_text()
parts = []
for stage, next_stage in [('RLCInit', 'SDMAInit'), ('MESInit', 'GFXInit'), ('GFXInit', 'IMUInit')]:
    start = src.index('    case BringupStage::' + stage + ':', src.index('static kern_return_t\nrun_stage'))
    end = src.index('    case BringupStage::' + next_stage + ':', start)
    parts.append(src[start:end])
start = src.index('    case BringupStage::CPInit: {', src.index('static kern_return_t\nrun_stage'))
end = src.index('\n    }\n    }', start) + len('\n    }')
parts.append(src[start:end])
Path('build/tests/cp_stages_under_test.inc').write_text('\n'.join(parts))
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I build/tests tests/cp_startup_test.cpp -o build/tests/cp-startup-test
build/tests/cp-startup-test
