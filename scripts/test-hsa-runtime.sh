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
required.update({
    'hsa_agent_iterate_regions', 'hsa_region_get_info', 'hsa_memory_allocate',
    'hsa_memory_free', 'hsa_memory_copy', 'hsa_amd_agent_iterate_memory_pools',
    'hsa_amd_memory_pool_get_info', 'hsa_amd_agent_memory_pool_get_info',
    'hsa_amd_memory_pool_allocate', 'hsa_amd_memory_pool_free',
    'hsa_amd_agents_allow_access', 'hsa_amd_memory_fill', 'hsa_amd_memory_async_copy',
    'hsa_amd_pointer_info', 'hsa_amd_pointer_info_set_userdata',
    'hsa_soft_queue_create', 'hsa_queue_destroy', 'hsa_queue_inactivate',
})
for suffix in ['relaxed', 'scacquire']:
    required.update('hsa_queue_load_' + index + '_index_' + suffix for index in ['read', 'write'])
for suffix in ['relaxed', 'screlease']:
    required.update('hsa_queue_store_' + index + '_index_' + suffix for index in ['read', 'write'])
for suffix in ['relaxed', 'scacquire', 'screlease', 'scacq_screl']:
    required.update('hsa_queue_' + op + '_write_index_' + suffix for op in ['add', 'cas'])
assert not required - exported, f'Missing implemented exports: {required - exported}'
print(f'{len(required)} signal, memory and software queue entry points exported from the built dylib')
PY
