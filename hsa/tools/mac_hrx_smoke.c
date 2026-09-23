// Exercise the real pinned HRX library rather than a symbol-count substitute.
#include "hrx_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hrx_compute_fixture.h"

static int check(hrx_status_t status, const char* operation) {
  if (hrx_status_is_ok(status)) return 1;
  char* message = NULL;
  size_t length = 0;
  hrx_status_t format_status = hrx_status_to_string(status, &message, &length);
  fprintf(stderr, "%s: %.*s (status=%d)\n", operation,
          message ? (int)length : 0, message ? message : "",
          (int)hrx_status_code(status));
  hrx_status_ignore(format_status);
  if (message) hrx_status_free_message(message);
  hrx_status_ignore(status);
  return 0;
}

static int compute(hrx_device_t device, hrx_stream_t stream) {
  enum { kElements = 4093, kGuard = 16, kWords = kElements + 2 * kGuard };
  const float sentinel = -8388607.0f;
  float a[kWords], b[kWords], expected[kWords], actual[kWords];
  hrx_buffer_t buffers[3] = {NULL, NULL, NULL};
  hrx_executable_t executable = NULL;
  int success = 0;
  if (!check(hrx_executable_load_data(device, hrx_compute_fixture,
              sizeof(hrx_compute_fixture), "amdgpu", "gfx1201", &executable),
             "load native HRX compute executable")) goto cleanup;
  for (size_t i = 0; i < 3; ++i) {
    if (!check(hrx_buffer_allocate(stream, sizeof(a), HRX_MEMORY_TYPE_DEVICE_LOCAL,
                 HRX_BUFFER_USAGE_DEFAULT, &buffers[i]),
               "allocate compute binding")) goto cleanup;
  }
  for (unsigned phase = 0; phase < 2; ++phase) {
    const size_t count = phase == 0 ? kElements : 256;
    const char* name = phase == 0 ? "hrx_vector_affine" : "hrx_matmul_16";
    uint32_t ordinal = UINT32_MAX;
    hrx_executable_export_info_t info = {0};
    if (!check(hrx_executable_lookup_export_by_name(executable, name, &ordinal),
               "lookup compute export") ||
        !check(hrx_executable_export_info(executable, ordinal, &info),
               "reflect compute export")) goto cleanup;
    if (info.binding_count != 3 || info.constant_byte_length != (phase == 0 ? 8u : 0u) ||
        info.workgroup_size[0] != 64 || info.workgroup_size[1] != 1 ||
        info.workgroup_size[2] != 1) {
      fprintf(stderr, "FAIL: unexpected HRX argument/workgroup ABI for %s\n", name);
      goto cleanup;
    }
    for (size_t i = 0; i < kWords; ++i) a[i] = b[i] = expected[i] = sentinel;
    for (size_t i = 0; i < count; ++i) {
      a[kGuard + i] = (float)((int)((i * 7u + 3u) % 31u) - 15);
      b[kGuard + i] = (float)((int)((i * 11u + 5u) % 29u) - 14);
    }
    if (!check(hrx_synchronous_h2d(device, a, buffers[0], 0, sizeof(a)), "upload compute A") ||
        !check(hrx_synchronous_h2d(device, b, buffers[1], 0, sizeof(b)), "upload compute B") ||
        !check(hrx_synchronous_h2d(device, expected, buffers[2], 0, sizeof(expected)),
               "initialize compute output guards")) goto cleanup;
    if (phase == 0) {
      for (size_t i = 0; i < count; ++i) expected[kGuard + i] = 3.0f * a[kGuard + i] + b[kGuard + i];
    } else {
      for (size_t row = 0; row < 16; ++row) {
        for (size_t column = 0; column < 16; ++column) {
          float value = 0.0f;
          for (size_t k = 0; k < 16; ++k) value += a[kGuard + row * 16 + k] * b[kGuard + k * 16 + column];
          expected[kGuard + row * 16 + column] = value;
        }
      }
    }
    const hrx_buffer_ref_t bindings[3] = {
      {buffers[0], kGuard * sizeof(float), count * sizeof(float)},
      {buffers[1], kGuard * sizeof(float), count * sizeof(float)},
      {buffers[2], kGuard * sizeof(float), count * sizeof(float)},
    };
    const hrx_dispatch_config_t config = {{(uint32_t)((count + 63) / 64), 1, 1}, {64, 1, 1}, 32};
    const struct { uint32_t count; float scale; } constants = {(uint32_t)count, 3.0f};
    if (!check(hrx_stream_dispatch(stream, executable, ordinal, &config,
                 phase == 0 ? &constants : NULL, phase == 0 ? sizeof(constants) : 0,
                 bindings, 3, HRX_DISPATCH_FLAG_NONE), "dispatch compute") ||
        !check(hrx_stream_synchronize(stream), "synchronize compute")) goto cleanup;
    for (size_t binding = 0; binding < 3; ++binding) {
      const float* reference = binding == 0 ? a : binding == 1 ? b : expected;
      if (!check(hrx_synchronous_d2h(device, buffers[binding], 0, actual, sizeof(actual)),
                 "readback full compute binding")) goto cleanup;
      for (size_t i = 0; i < kWords; ++i) {
        if (actual[i] != reference[i]) {
          fprintf(stderr, "FAIL: %s binding=%zu word=%zu expected=%.9g actual=%.9g\n",
                  name, binding, i, (double)reference[i], (double)actual[i]);
          goto cleanup;
        }
      }
    }
    printf("PASS: HRX %s, %zu exact FP32 outputs; inputs and all guard/tail words unchanged.\n",
           name, count);
  }
  success = 1;
cleanup:
  for (size_t i = 0; i < 3; ++i) if (buffers[i]) hrx_buffer_release(buffers[i]);
  if (executable) hrx_executable_release(executable);
  return success;
}

int main(int argc, char** argv) {
  if (argc != 2 || (strcmp(argv[1], "--check-library") &&
                    strcmp(argv[1], "--run"))) {
    fprintf(stderr, "Usage: %s --check-library|--run\n", argv[0]);
    return 2;
  }
  // This branch does not initialize HSA, enumerate devices, or submit GPU work.
  if (!strcmp(argv[1], "--check-library")) {
    hrx_status_t status = hrx_make_status(HRX_STATUS_INVALID_ARGUMENT,
                                        "macOS HRX library loading check");
    if (hrx_status_code(status) != HRX_STATUS_INVALID_ARGUMENT) return 1;
    hrx_status_ignore(status);
    puts("PASS: real HRX dylib loads; status ABI works. No GPU operations run.");
    return 0;
  }
  hrx_device_t device = NULL;
  hrx_stream_t stream = NULL;
  hrx_buffer_t src = NULL, dst = NULL;
  int initialized = 0, success = 0, count = 0;
  enum { kBytes = 65536 };
  unsigned char input[kBytes], output[kBytes];
  for (size_t i = 0; i < kBytes; ++i) input[i] = (unsigned char)(i * 29u + (i >> 7));
  if (!check(hrx_gpu_initialize(0), "hrx_gpu_initialize")) goto cleanup;
  initialized = 1;
  if (!check(hrx_gpu_device_count(&count), "hrx_gpu_device_count")) goto cleanup;
  if (count != 1) { fprintf(stderr, "Expected one GPU; found %d\n", count); goto cleanup; }
  if (!check(hrx_gpu_device_get(0, &device), "hrx_gpu_device_get") ||
      !check(hrx_stream_create(device, 0, &stream), "hrx_stream_create") ||
      !check(hrx_buffer_allocate(stream, kBytes, HRX_MEMORY_TYPE_DEVICE_LOCAL,
                                 HRX_BUFFER_USAGE_DEFAULT, &src), "allocate src") ||
      !check(hrx_buffer_allocate(stream, kBytes, HRX_MEMORY_TYPE_DEVICE_LOCAL,
                                 HRX_BUFFER_USAGE_DEFAULT, &dst), "allocate dst")) goto cleanup;
  if (!check(hrx_synchronous_h2d(device, input, src, 0, kBytes), "upload") ||
      !check(hrx_stream_copy_buffer(stream, src, 0, dst, 0, kBytes), "copy") ||
      !check(hrx_stream_synchronize(stream), "synchronize copy") ||
      !check(hrx_synchronous_d2h(device, dst, 0, output, kBytes), "readback")) goto cleanup;
  if (memcmp(input, output, kBytes)) { fprintf(stderr, "FAIL: HRX copy mismatch\n"); goto cleanup; }
  const unsigned char pattern = 0x5a;
  if (!check(hrx_stream_fill_buffer(stream, dst, 0, kBytes, &pattern, 1), "fill") ||
      !check(hrx_stream_synchronize(stream), "synchronize fill") ||
      !check(hrx_synchronous_d2h(device, dst, 0, output, kBytes), "fill readback")) goto cleanup;
  for (size_t i = 0; i < kBytes; ++i) {
    if (output[i] != pattern) { fprintf(stderr, "FAIL: HRX fill mismatch at %zu\n", i); goto cleanup; }
  }
  if (!compute(device, stream)) goto cleanup;
  success = 1;
cleanup:
  if (dst) hrx_buffer_release(dst);
  if (src) hrx_buffer_release(src);
  if (stream) hrx_stream_release(stream);
  if (initialized && !check(hrx_gpu_shutdown(), "hrx_gpu_shutdown")) success = 0;
  if (success) puts("PASS: HRX initialization, streams, 64 KiB copy/fill, vector/matrix compute, guarded readbacks and shutdown.");
  return success ? 0 : 1;
}
