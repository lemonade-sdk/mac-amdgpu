#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash scripts/test-hsa-code-objects.sh
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
    'hsa_agent_iterate_caches', 'hsa_cache_get_info',
    'hsa_amd_memory_lock', 'hsa_amd_memory_lock_to_pool', 'hsa_amd_memory_unlock',
    'hsa_amd_ipc_memory_create', 'hsa_amd_ipc_memory_attach', 'hsa_amd_ipc_memory_detach',
    'hsa_amd_ipc_signal_create', 'hsa_amd_ipc_signal_attach',
    'hsa_amd_vmem_address_reserve_align', 'hsa_amd_vmem_address_free',
    'hsa_amd_vmem_handle_create', 'hsa_amd_vmem_handle_release',
    'hsa_amd_vmem_map', 'hsa_amd_vmem_unmap', 'hsa_amd_vmem_set_access',
    'hsa_amd_register_system_event_handler', 'hsa_amd_profiling_set_profiler_enabled',
    'hsa_amd_queue_get_info', 'hsa_amd_queue_cu_set_mask', 'hsa_amd_queue_set_priority',
    'hsa_amd_interop_map_buffer', 'hsa_amd_interop_unmap_buffer',
    'hsa_amd_portable_export_dmabuf', 'hsa_amd_portable_close_dmabuf',
    'hsa_amd_svm_attributes_get', 'hsa_amd_svm_attributes_set', 'hsa_amd_svm_prefetch_async',
    'hsa_code_object_reader_create_from_memory', 'hsa_code_object_reader_destroy',
    'hsa_executable_create_alt', 'hsa_executable_destroy',
    'hsa_executable_load_agent_code_object', 'hsa_executable_freeze',
    'hsa_executable_validate_alt', 'hsa_executable_get_symbol_by_name',
    'hsa_executable_symbol_get_info',
    'hsa_ven_amd_loader_query_host_address', 'hsa_ven_amd_loader_query_segment_descriptors',
    'hsa_ven_amd_loader_query_executable', 'hsa_ven_amd_loader_executable_iterate_loaded_code_objects',
    'hsa_ven_amd_loader_loaded_code_object_get_info',
    'hsa_ven_amd_loader_code_object_reader_create_from_file_with_offset_size',
    'hsa_ven_amd_loader_iterate_executables',
    'hsa_agent_iterate_isas', 'hsa_isa_get_info_alt',
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
print(f'{len(required)} checked entry points exported; API_STATUS.md records behavioral limits')
PY
python3 hsa/tools/audit_hrx.py --hrx upstream/hrx-lse-pin --library build/hsa/libhsa-runtime64.dylib
