// Match HRX's device-side scoped atomics, including system scope.
__attribute__((reqd_work_group_size(32,1,1)))
__kernel void signal_add(__global long *data, unsigned iterations) {
    unsigned lane=__builtin_amdgcn_workitem_id_x();
    for (unsigned i=0;i<iterations;++i) {
        long ticket=__scoped_atomic_fetch_add(data,1L,__ATOMIC_ACQ_REL,__MEMORY_SCOPE_SYSTEM);
        data[64+lane]=ticket;
    }
}
__attribute__((reqd_work_group_size(32,1,1)))
__kernel void signal_publish(__global long *data, unsigned iterations) {
    if (__builtin_amdgcn_workitem_id_x()) return;
    for (unsigned i=1;i<=iterations;++i) {
        data[64+i]=0x6143210000000000L+i;
        __scoped_atomic_store_n(data,(long)i,__ATOMIC_RELEASE,__MEMORY_SCOPE_SYSTEM);
    }
}
// A shared AMD signal value may be followed immediately by other signals.
// This kernel touches only that one 64-bit value.
__attribute__((reqd_work_group_size(32,1,1)))
__kernel void signal_accumulate(__global long *value, unsigned iterations) {
    for (unsigned i=0;i<iterations;++i)
        __scoped_atomic_fetch_add(value,1L,__ATOMIC_ACQ_REL,__MEMORY_SCOPE_SYSTEM);
}
