#include "mac_hsa.h"
#include <hsa/hsa_ext_amd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

static hsa_agent_t gpu;
static hsa_amd_memory_pool_t device_pool;
static hsa_status_t agent(hsa_agent_t candidate, void *unused) {
    (void)unused;
    hsa_device_type_t type;
    hsa_status_t status = hsa_agent_get_info(candidate, HSA_AGENT_INFO_DEVICE, &type);
    if (!status && type == HSA_DEVICE_TYPE_GPU && !gpu.handle) gpu = candidate;
    return status;
}
static hsa_status_t pool(hsa_amd_memory_pool_t candidate, void *unused) {
    (void)unused; device_pool = candidate; return HSA_STATUS_INFO_BREAK;
}
static int check(hsa_status_t status, const char *step) {
    if (!status) return 1;
    fprintf(stderr, "%s failed: HSA status %#x\n", step, (unsigned)status); return 0;
}
int main(int argc, char **argv) {
    if (argc != 3 || (strcmp(argv[1], "--run") && strcmp(argv[1], "--shared"))) {
        fprintf(stderr, "Usage: %s --run|--shared build/tests/hsa-code-object.hsaco (driver 183+)\n", argv[0]); return 2;
    }
    const int shared = !strcmp(argv[1], "--shared");
    setvbuf(stdout, NULL, _IONBF, 0);
    FILE *file = fopen(argv[2], "rb");
    if (!file) { perror("open code object"); return 1; }
    if (fseek(file, 0, SEEK_END)) { fclose(file); return 1; }
    long length = ftell(file);
    if (length <= 0 || length > 256 * 1024 * 1024 || fseek(file, 0, SEEK_SET)) { fclose(file); return 1; }
    void *bytes = malloc((size_t)length);
    if (!bytes) { fclose(file); return 1; }
    size_t count = fread(bytes, 1, (size_t)length, file); fclose(file);
    if (count != (size_t)length || !check(hsa_init(), "runtime init")) { free(bytes); return 1; }
    hsa_code_object_reader_t reader = {0};
    hsa_executable_t executable = {0};
    hsa_executable_symbol_t symbol = {0};
    void *data = NULL;
    int passed = 0;
    uint32_t initial[4096], expected[4096], observed[4096];
    uint64_t previous_fence = 0;
    mac_hsa_device_info_t device = {0};
    if (!check(hsa_iterate_agents(agent, NULL), "enumerate GPU") || !gpu.handle) goto cleanup;
    if (!check(mac_hsa_agent_get_driver_info(gpu, &device, sizeof(device)), "driver build") || device.driver_build < 183) {
        fputs("Install driver 0.1.83 before running the kernel test. No GPU work submitted.\n", stderr); goto cleanup;
    }
    if (hsa_amd_agent_iterate_memory_pools(gpu, pool, NULL) != HSA_STATUS_INFO_BREAK) goto cleanup;
    if (!check(hsa_code_object_reader_create_from_memory(bytes, count, &reader), "reader")) goto cleanup;
    if (!check(hsa_executable_create_alt(HSA_PROFILE_BASE, HSA_DEFAULT_FLOAT_ROUNDING_MODE_DEFAULT, NULL, &executable), "executable")) goto cleanup;
    puts("Loading kernel: automatically initialize the GPU or join its ready session.");
    if (!check(hsa_executable_load_agent_code_object(executable, gpu, reader, NULL, NULL), "load code")) goto cleanup;
    if (!check(hsa_code_object_reader_destroy(reader), "release reader")) goto cleanup;
    reader.handle = 0;
    if (!check(hsa_executable_freeze(executable, NULL), "freeze")) goto cleanup;
    if (!check(hsa_executable_get_symbol_by_name(executable, "vector_add.kd", &gpu, &symbol), "kernel symbol")) goto cleanup;
    if (!check(shared ? mac_hsa_memory_allocate_shared(gpu, sizeof(initial), &data) :
        hsa_amd_memory_pool_allocate(device_pool, sizeof(initial), 0, &data), "kernel data")) goto cleanup;
    printf("Data allocation=%p (%s)\n", data, shared ? "identical CPU/GPU address" : "device VRAM");
    for (uint32_t group_count = 4; group_count <= 8; group_count += 4) {
        const uint32_t seed = 0x6ba18937u ^ group_count;
        for (size_t i = 0; i < 4096; ++i) initial[i] = (uint32_t)i * 0x10203041u ^ seed;
        memcpy(expected, initial, sizeof(expected));
        for (size_t i = 0; i < group_count * 32; ++i) expected[256 + i] = initial[i] + seed;
        if (shared) { memcpy(data, initial, sizeof(initial)); atomic_thread_fence(memory_order_seq_cst); }
        else if (!check(hsa_memory_copy(data, initial, sizeof(initial)), "upload inputs and guards")) goto cleanup;
        unsigned char args[12];
        uint64_t address = (uint64_t)(uintptr_t)data;
        memcpy(args, &address, 8); memcpy(args + 8, &seed, 4);
        const uint32_t groups[3] = {group_count, 1, 1}, threads[3] = {32, 1, 1};
        const void *buffers[] = {data};
        uint64_t fence = 0;
        if (!check(mac_hsa_executable_dispatch(symbol, args, sizeof(args), groups, threads, buffers, 1, &fence), "launch HSA-loaded kernel")) goto cleanup;
        if (fence <= previous_fence) { fputs("Non-increasing GPU fence\n", stderr); goto cleanup; }
        previous_fence = fence;
        if (shared) { atomic_thread_fence(memory_order_seq_cst); memcpy(observed, data, sizeof(observed)); }
        else if (!check(hsa_memory_copy(observed, data, sizeof(observed)), "download GPU result")) goto cleanup;
        for (size_t i = 0; i < 4096; ++i) {
            if (observed[i] != expected[i]) {
                fprintf(stderr, "Mismatch at word %zu: %#x expected %#x\n", i, observed[i], expected[i]); goto cleanup;
            }
        }
        printf("PASS: %u computed outputs; all 16384 input/output/guard bytes verified; GPU fence=%llu\n", group_count * 32, (unsigned long long)fence);
    }
    passed = 1;
cleanup:
    if (data && !check(hsa_memory_free(data), "free data")) passed = 0;
    if (executable.handle && !check(hsa_executable_destroy(executable), "destroy executable")) passed = 0;
    if (reader.handle && !check(hsa_code_object_reader_destroy(reader), "destroy reader")) passed = 0;
    if (!check(hsa_shut_down(), "runtime shutdown")) passed = 0;
    free(bytes);
    puts("Native synchronous launch only: hardware AQL queues and HRX inference are not tested.");
    return passed ? 0 : 1;
}
