// CPU-requested signal updates execute on the GPU so every writer shares the
// GPU atomic domain. The CPU only observes the shared value with acquire loads.
__attribute__((reqd_work_group_size(32,1,1)))
__kernel void signal_operation(__global long *signal, __global long *result,
    long value, long compare, unsigned operation) {
    if (__builtin_amdgcn_workitem_id_x()) return;
    long old=0;
    switch (operation) {
    case 1: case 7:
        old=__scoped_atomic_exchange_n(signal,value,__ATOMIC_SEQ_CST,__MEMORY_SCOPE_SYSTEM);break;
    case 2: old=__scoped_atomic_fetch_add(signal,value,__ATOMIC_SEQ_CST,__MEMORY_SCOPE_SYSTEM);break;
    case 3: old=__scoped_atomic_fetch_sub(signal,value,__ATOMIC_SEQ_CST,__MEMORY_SCOPE_SYSTEM);break;
    case 4: old=__scoped_atomic_fetch_and(signal,value,__ATOMIC_SEQ_CST,__MEMORY_SCOPE_SYSTEM);break;
    case 5: old=__scoped_atomic_fetch_or(signal,value,__ATOMIC_SEQ_CST,__MEMORY_SCOPE_SYSTEM);break;
    case 6: old=__scoped_atomic_fetch_xor(signal,value,__ATOMIC_SEQ_CST,__MEMORY_SCOPE_SYSTEM);break;
    case 8:
        old=compare;
        __scoped_atomic_compare_exchange_n(signal,&old,value,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST,__MEMORY_SCOPE_SYSTEM);break;
    }
    result[0]=old;
}
