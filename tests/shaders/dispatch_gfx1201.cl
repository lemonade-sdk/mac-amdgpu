__attribute__((reqd_work_group_size(32,1,1)))
__kernel void vector_add(__global unsigned *data, unsigned seed) {
    unsigned i=__builtin_amdgcn_workgroup_id_x()*32+__builtin_amdgcn_workitem_id_x();
    data[256+i]=data[i]+seed;
}
