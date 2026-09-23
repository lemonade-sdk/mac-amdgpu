#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
import re
s=Path('upstream/linux/drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c').read_text()
a=s.index('static int gfx_v12_0_gfx_mqd_init('); b=s.index('\n}',a)+2
body=s[a:b].replace('static int gfx_v12_0_gfx_mqd_init(struct amdgpu_device *adev, void *m,','static int linux_gfx_mqd_init(void *m,')
body=body.replace('struct v12_gfx_mqd *mqd = m;', 'struct v12_gfx_mqd *mqd = static_cast<v12_gfx_mqd *>(m);')
constants='\n'.join(re.findall(r'^#define regCP_\w+_DEFAULT\s+0x[0-9a-fA-F]+',s,re.M))
Path('build/tests/linux_gfx_mqd_reference.inc').write_text(constants+'\n'+body)
h=Path('dext/amdgpu/amdgpu_gfx_mqd.h').read_text()
fields=re.findall(r'constexpr uint32_t (\w+) =',h)
Path('build/tests/gfx_mqd_offsets.inc').write_text('\n'.join(f'static_assert(GFXMQDOff::{f}*4 == offsetof(v12_gfx_mqd,{f}));' for f in fields))
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I dext/amdgpu -I build/tests tests/gfx_mqd_test.cpp -o build/tests/gfx-mqd-test
build/tests/gfx-mqd-test
