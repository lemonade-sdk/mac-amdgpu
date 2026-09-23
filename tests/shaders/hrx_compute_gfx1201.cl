// Native code-object exports consumed by HRX's public executable API.
// Integer-valued FP32 operands keep every expected result exactly representable.
__attribute__((reqd_work_group_size(64, 1, 1)))
__kernel void hrx_vector_affine(__global const float* a,
                                __global const float* b,
                                __global float* output, uint count,
                                float scale) {
  uint i = __builtin_amdgcn_workgroup_id_x() * 64u +
           __builtin_amdgcn_workitem_id_x();
  if (i < count) output[i] = a[i] * scale + b[i];
}

__attribute__((reqd_work_group_size(64, 1, 1)))
__kernel void hrx_matmul_16(__global const float* a,
                           __global const float* b,
                           __global float* output) {
  uint i = __builtin_amdgcn_workgroup_id_x() * 64u +
           __builtin_amdgcn_workitem_id_x();
  if (i >= 256u) return;
  uint row = i / 16u, column = i % 16u;
  float value = 0.0f;
  for (uint k = 0; k < 16u; ++k) value += a[row * 16u + k] * b[k * 16u + column];
  output[i] = value;
}
