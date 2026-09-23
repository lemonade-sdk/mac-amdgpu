// Exercise private scratch spills, static LDS, barriers and compiler implicit arguments using an ordinary compiler-produced gfx1201 descriptor.
__attribute__((reqd_work_group_size(32,1,1)))
__kernel void scratch_lds(__global unsigned *output, unsigned seed) {
    unsigned lane=__builtin_amdgcn_workitem_id_x();
    volatile unsigned temporary[64];
    __local unsigned shared[32];
    for (unsigned i=0;i<64;++i) temporary[i]=seed+lane+i;
    shared[lane]=temporary[(lane+7)&63];
    __builtin_amdgcn_s_barrier();
    output[__builtin_amdgcn_workgroup_id_x()*32+lane]=shared[(lane+1)&31]+temporary[(lane+11)&63];
}
