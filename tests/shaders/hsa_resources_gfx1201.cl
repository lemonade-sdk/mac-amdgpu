// Exercise private scratch spills, static LDS, barriers and compiler implicit arguments using an ordinary compiler-produced gfx1201 descriptor.
__attribute__((reqd_work_group_size(32,1,1)))
__kernel void scratch_lds(__global unsigned *output, unsigned seed) {
    unsigned lane=__builtin_amdgcn_workitem_id_x();
    volatile unsigned temporary[64];
    __local unsigned shared[32];
    for (unsigned i=0;i<64;++i) temporary[i]=seed+lane+i;
    shared[lane]=temporary[(lane+7)&63];
    // An execution barrier alone does not publish LDS stores to other lanes.
    __builtin_amdgcn_fence(__ATOMIC_RELEASE, "workgroup");
    __builtin_amdgcn_s_barrier();
    __builtin_amdgcn_fence(__ATOMIC_ACQUIRE, "workgroup");
    output[__builtin_amdgcn_workgroup_id_x()*32+lane]=shared[(lane+1)&31]+temporary[(lane+11)&63];
}

// Diagnostic exports separate lane indexing, scratch addressing and LDS.
__attribute__((reqd_work_group_size(32,1,1)))
__kernel void resource_ids(__global unsigned *output, unsigned seed) {
    unsigned lane=__builtin_amdgcn_workitem_id_x();
    unsigned group=__builtin_amdgcn_workgroup_id_x();
    output[group*32+lane]=0xa5000000u|(group<<8)|lane;
}
__attribute__((reqd_work_group_size(32,1,1)))
__kernel void resource_scratch(__global unsigned *output, unsigned seed) {
    unsigned lane=__builtin_amdgcn_workitem_id_x();
    unsigned group=__builtin_amdgcn_workgroup_id_x();
    volatile unsigned temporary[64];
    for (unsigned i=0;i<64;++i) temporary[i]=seed+group*1024+lane+i;
    output[group*32+lane]=temporary[(lane+11)&63];
}
__attribute__((reqd_work_group_size(32,1,1)))
__kernel void resource_lds(__global unsigned *output, unsigned seed) {
    unsigned lane=__builtin_amdgcn_workitem_id_x();
    unsigned group=__builtin_amdgcn_workgroup_id_x();
    __local unsigned shared[32];
    shared[lane]=seed+group*1024+lane;
    __builtin_amdgcn_fence(__ATOMIC_RELEASE, "workgroup");
    __builtin_amdgcn_s_barrier();
    __builtin_amdgcn_fence(__ATOMIC_ACQUIRE, "workgroup");
    output[group*32+lane]=shared[(lane+1)&31];
}
