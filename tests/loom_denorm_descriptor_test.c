// Host-only regression of the shipping binary descriptor writer.
#include "loom/target/emit/native/amdgpu/descriptor.h"
// Exercise the actual private assembly-metadata printer without exporting a
// test-only ABI. Its public entry point is renamed to avoid duplicate linkage.
#define loom_amdgpu_emit_kernel_assembly qualification_emit_kernel_assembly
#include "loom/target/emit/native/amdgpu/kernel_assembly.c"
#undef loom_amdgpu_emit_kernel_assembly
#include "loom/target/arch/amdgpu/target_info.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static int ok(iree_status_t s) {
  if (iree_status_is_ok(s))
    return 1;
  iree_status_fprint(stderr, s);
  iree_status_ignore(s);
  return 0;
}
int main(void) {
  unsigned checked = 0;
  for (iree_host_size_t i = 0; i < loom_amdgpu_target_info_processor_count();
       ++i) {
    const loom_amdgpu_processor_info_t *processor =
        loom_amdgpu_target_info_processor_at(i);
    if (processor->properties.kernel_descriptor.profile ==
        LOOM_AMDGPU_KERNEL_DESCRIPTOR_PROFILE_NONE)
      continue;
    ++checked;
    loom_amdgpu_metadata_kernel_t m = {0};
    m.name = iree_make_cstring_view("denorm_test");
    m.descriptor_symbol = iree_make_cstring_view("denorm_test.kd");
    m.kernarg_segment_alignment = 8;
    m.wavefront_size = processor->properties.wavefront.default_size;
    m.max_flat_workgroup_size = 64;
    m.sgpr_count = 20;
    m.vgpr_count = 9;
    loom_amdgpu_kernel_descriptor_t d = {0};
    if (!ok(loom_amdgpu_kernel_descriptor_initialize_from_metadata(
            processor->name, &m, 0, &d)))
      return 1;
    uint8_t bytes[64] = {0};
    if (!ok(loom_amdgpu_kernel_descriptor_write(
            &d, iree_make_byte_span(bytes, sizeof bytes))))
      return 1;
    uint32_t r = 0;
    for (unsigned b = 0; b < 4; ++b)
      r |= (uint32_t)bytes[48 + b] << (b * 8);
    if (((r >> 16) & 3) != 3 || ((r >> 18) & 3) != 3 || ((r >> 12) & 15) != 0) {
      fprintf(stderr, "FAIL %s rsrc1=0x%08x\n", processor->name.data, r);
      return 2;
    }
    loom_amdgpu_kernel_record_t record = {0};
    record.processor = processor;
    record.metadata = m;
    record.symbol = m.name;
    record.descriptor_symbol = m.descriptor_symbol;
    char target[128];
    snprintf(target, sizeof target, "amdgcn-amd-amdhsa--%.*s",
             (int)processor->name.size, processor->name.data);
    record.code_object_target_id = iree_make_cstring_view(target);
    iree_string_builder_t builder;
    iree_string_builder_initialize(iree_allocator_system(), &builder);
    if (!ok(loom_amdgpu_kernel_assembly_append_metadata(&record, &builder)))
      return 3;
    iree_string_view_t assembly = iree_string_builder_view(&builder);
    const char *directives[] = {".amdhsa_float_denorm_mode_32 3\n",
                                ".amdhsa_float_denorm_mode_16_64 3\n",
                                ".amdhsa_float_round_mode_32 0\n",
                                ".amdhsa_float_round_mode_16_64 0\n"};
    for (unsigned j = 0; j < 4; ++j)
      if (iree_string_view_find(assembly, iree_make_cstring_view(directives[j]),
                                0) == IREE_STRING_VIEW_NPOS) {
        fprintf(stderr, "FAIL assembly %s %s", processor->name.data,
                directives[j]);
        return 4;
      }
    iree_string_builder_deinitialize(&builder);
    printf("PASS %s rsrc1=0x%08x denorm32=3 denorm16_64=3 round32=0 "
           "round16_64=0\n",
           processor->name.data, r);
  }
  if (!checked)
    return 5;
  printf(
      "PASS binary/assembly floating-mode parity for %u processor profiles\n",
      checked);
  return 0;
}
