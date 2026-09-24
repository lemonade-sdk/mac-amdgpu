#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/tests
python3 - <<'PY'
from pathlib import Path
source = Path('dext/MacAMDGPU.cpp').read_text()
start_body = source[source.index('IMPL(MacAMDGPU, Start)'):source.index('// MacAMDGPU::Stop')]
assert 'ConfigurationRead' not in start_body, 'Start must not read configuration before Open'
identity_query = source[source.index('        case 6: { // Owned initialized session'):source.index('        case 7: { // Owned shared-memory')]
assert '!driver->ivars->deviceID || driver->ivars->deviceID==UINT16_MAX' in identity_query, 'Topology query must reject invalid cached identity'

start = source.index('static kern_return_t\nmac_amdgpu_admit_external(')
end = source.index('static void\nmac_amdgpu_release_dma_state(', start)
Path('build/tests/client_admission_under_test.inc').write_text(source[start:end])
start = source.index('static uint8_t\nmac_amdgpu_find_pm_capability(')
end = source.index('// Open the PCI device (idempotent)', start)
Path('build/tests/client_pm_cap_under_test.inc').write_text(source[start:end])
start = source.index('static kern_return_t\nmac_amdgpu_ensure_open(')
end = source.index('    uint16_t cmd = 0;', start)
Path('build/tests/client_open_under_test.inc').write_text(source[start:end] + '    return kIOReturnSuccess;\n}\n')
a = source.index('static void mac_amdgpu_raw_work_completed(')
b = source.index('//\n// BO helpers', a)
Path('build/tests/client_software_raw_under_test.inc').write_text(source[a:b])
sections = []
for name, next_name in [('WaitInterrupt', 'SetIRQMask'), ('WaitFence', 'CSCreate'),
                        ('SubmitTestPM4', 'CPKIQSmoke'), ('CPKIQSmoke', 'SDMACopyTest'),
                        ('SDMACopyTest', 'SDMACopyVRAM')]:
    start = source.index('    case kMacAMDGPUMethod' + name + ':')
    end = source.index('    case kMacAMDGPUMethod' + next_name + ':', start)
    sections.append(source[start:end])
Path('build/tests/client_rpc_lifecycle_under_test.inc').write_text('\n'.join(sections))
start = source.index('    case kMacAMDGPUMethodSubmitIB:')
end = source.index('        // Legacy BO-handle fallback (pre-v0.1.28 callers).', start)
Path('build/tests/client_cs_submit_under_test.inc').write_text(
    source[start:end] + '\nreturn kIOReturnBadArgument;\n}\n')
start = source.index('        // Legacy BO-handle fallback (pre-v0.1.28 callers).')
end = source.index('        IOAddressSegment seg = {};', start)
Path('build/tests/client_legacy_validation_under_test.inc').write_text(source[start:end])
PY
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I build/tests tests/client_lifecycle_test.cpp -o build/tests/client-lifecycle-test
build/tests/client-lifecycle-test
xcrun clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I build/tests tests/client_rpc_lifecycle_test.cpp -o build/tests/client-rpc-lifecycle-test
build/tests/client-rpc-lifecycle-test
