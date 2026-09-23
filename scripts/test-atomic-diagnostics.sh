#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
s=Path('dext/MacAMDGPU.cpp').read_text()
a=s.index('        case 7: { // Owned shared-memory atomic configuration snapshot')
b=s.index('        default:',a)
body=s[a:b]
assert 'ConfigurationWrite' not in body and 'WREG' not in body and 'MemoryWrite' not in body
Path('build/tests/atomic_diagnostics_rpc.inc').write_text(body)
linux=Path('upstream/linux/drivers/gpu/drm/amd/include/v12_structs.h').read_text()
# Both the production MQD builder and this readback must point at the same field.
assert 'cp_hqd_hq_status0=160' in Path('dext/amdgpu/amdgpu_aql_packets.h').read_text()
assert 'cp_hqd_hq_status0' in linux
assert 'DMA_BIT_MASK(44)' in Path('upstream/linux/drivers/gpu/drm/amd/amdgpu/gmc_v12_0.c').read_text()
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I dext/amdgpu -I build/tests tests/atomic_diagnostics_test.cpp -o build/tests/atomic-diagnostics-test
build/tests/atomic-diagnostics-test
