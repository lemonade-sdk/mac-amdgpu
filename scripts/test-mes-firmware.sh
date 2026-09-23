#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
s = Path('dext/MacAMDGPU.cpp').read_text()
a=s.index('                uint64_t mesEntryAddress = 0;')
b=s.index('                // Submit each payload',a)
pre=s[a:b]
a=s.index('                    if (isMESPackage) {',b)
b=s.index('                    if (payloads[i].fw_type == amdgpu::PSPGfxFwType::SDMA0',a)
ack=s[a:b]
a=s.index('                if (isMESPackage) {',b)
b=s.index('                if (cpFirmwareIndex >= 0)',a)
post=s[a:b]
Path('build/tests/mes_firmware_gate_under_test.inc').write_text(pre+'''
    for (size_t i=0; i<payloads.size(); ++i) {
        // Model the PSP return at the production acknowledgement boundary.
        assert(!driver->ivars->bringup.mes.sched_ucode_loaded);
        assert(!driver->ivars->bringup.mes.kiq_ucode_loaded);
        if (i == failedPayload) return kIOReturnIOError;
'''+ack+'    }\n'+post+'\n    return kIOReturnSuccess;\n')
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I dext/amdgpu -I build/tests tests/mes_firmware_test.cpp -o build/tests/mes-firmware-test
build/tests/mes-firmware-test
