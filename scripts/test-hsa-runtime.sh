#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S hsa -B build/hsa -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/hsa --parallel 4
ctest --test-dir build/hsa --output-on-failure
python3 - <<'PY'
import re, subprocess
symbols = subprocess.check_output(['nm', '-gU', 'build/hsa/libhsa-runtime64.dylib'], text=True)
exported = set(re.findall(r'\b_(hsa_\w+)$', symbols, re.M))
required = {'hsa_signal_create', 'hsa_signal_destroy', 'hsa_amd_signal_create',
            'hsa_amd_signal_wait_all', 'hsa_amd_signal_wait_any'}
for suffix in ['relaxed', 'scacquire']:
    required.update('hsa_signal_' + op + '_' + suffix for op in ['load', 'wait'])
for suffix in ['relaxed', 'screlease']:
    required.update('hsa_signal_' + op + '_' + suffix for op in ['store', 'silent_store'])
for suffix in ['relaxed', 'scacquire', 'screlease', 'scacq_screl']:
    required.update('hsa_signal_' + op + '_' + suffix
                    for op in ['add', 'subtract', 'and', 'or', 'xor', 'exchange', 'cas'])
assert not required - exported, f'Missing signal exports: {required - exported}'
print(f'{len(required)} signal entry points exported from the built dylib')
PY
