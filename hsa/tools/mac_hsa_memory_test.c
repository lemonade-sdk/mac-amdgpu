#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static hsa_agent_t cpu, gpu;
static hsa_amd_memory_pool_t device_pool;
static hsa_status_t agent(hsa_agent_t handle, void *unused) {
    (void)unused;
    hsa_device_type_t type;
    hsa_status_t status = hsa_agent_get_info(handle, HSA_AGENT_INFO_DEVICE, &type);
    if (status != HSA_STATUS_SUCCESS) return status;
    if (type == HSA_DEVICE_TYPE_CPU) cpu = handle;
    if (type == HSA_DEVICE_TYPE_GPU && !gpu.handle) gpu = handle;
    return HSA_STATUS_SUCCESS;
}
static hsa_status_t pool(hsa_amd_memory_pool_t handle, void *unused) {
    (void)unused; device_pool = handle; return HSA_STATUS_INFO_BREAK;
}
static int check(hsa_status_t status, const char *step) {
    if (status == HSA_STATUS_SUCCESS) return 1;
    fprintf(stderr, "%s failed: HSA status 0x%x\n", step, (unsigned)status); return 0;
}
int main(int argc, char **argv) {
    if (argc != 2 || (strcmp(argv[1], "--run") && strcmp(argv[1], "--hold"))) {
        fprintf(stderr, "Usage: %s --run|--hold\nBuild179 requires Host Stop first. Build180 can share an initialized GPU.\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!check(hsa_init(), "initialize runtime")) return 1;
    void *a = NULL, *b = NULL;
    hsa_signal_t completed = {0};
    int passed = 0;
    unsigned char input[12003], output[16384];
    for (size_t i = 0; i < sizeof(input); ++i) input[i] = (unsigned char)(i * 79 + 13);
    if (!check(hsa_iterate_agents(agent, NULL), "enumerate agents") || !cpu.handle || !gpu.handle) goto cleanup;
    hsa_status_t status = hsa_amd_agent_iterate_memory_pools(gpu, pool, NULL);
    if (status != HSA_STATUS_INFO_BREAK || !device_pool.handle) goto cleanup;
    puts("Joining GPU session (initializing firmware only if needed)...");
    size_t capacity = 0;
    if (!check(hsa_amd_memory_pool_get_info(device_pool, HSA_AMD_MEMORY_POOL_INFO_SIZE, &capacity), "GPU pool capacity")) goto cleanup;
    printf("GPU session ready; device pool capacity=%zu bytes\n", capacity);
    if (!check(hsa_amd_memory_pool_allocate(device_pool, sizeof(output), 0, &a), "allocate A") ||
        !check(hsa_amd_memory_pool_allocate(device_pool, sizeof(output), 0, &b), "allocate B")) goto cleanup;
    if (!check(hsa_amd_memory_fill(a, 0x91919191, sizeof(output) / 4), "fill GPU guards")) goto cleanup;
    if (!check(hsa_memory_copy((void *)((uintptr_t)a + 3), input, sizeof(input)), "unaligned upload")) goto cleanup;
    if (!strcmp(argv[1], "--hold")) {
        printf("READY: GPU buffers A=%p B=%p hold verified input; run another client, then press Enter to verify survival\n", a, b);
        if (getchar() == EOF) goto cleanup;
    }
    if (!check(hsa_memory_copy(b, a, sizeof(output)), "device copy")) goto cleanup;
    if (!check(hsa_signal_create(1, 1, &cpu, &completed), "CPU completion signal")) goto cleanup;
    if (!check(hsa_amd_memory_async_copy(output, cpu, b, gpu, sizeof(output), 0, NULL, completed), "async download")) goto cleanup;
    hsa_signal_value_t value = hsa_signal_wait_scacquire(completed, HSA_SIGNAL_CONDITION_LT, 1,
                                                       10000000000ull, HSA_WAIT_STATE_BLOCKED);
    if (value != 0) { fprintf(stderr, "async completion value=%lld\n", (long long)value); goto cleanup; }
    for (size_t i = 0; i < sizeof(output); ++i) {
        unsigned char expected = i >= 3 && i < sizeof(input) + 3 ? input[i - 3] : 0x91;
        if (output[i] != expected) {
            fprintf(stderr, "byte %zu: got 0x%02x expected 0x%02x\n", i, output[i], expected); goto cleanup;
        }
    }
    puts("PASS: 12003 unaligned data bytes and all 4381 guard bytes; GPU fill/copy and asynchronous completion verified");
    passed = 1;
cleanup:
    if (completed.handle) hsa_signal_destroy(completed);
    if (b && !check(hsa_amd_memory_pool_free(b), "free B")) passed = 0;
    if (a && !check(hsa_amd_memory_pool_free(a), "free A")) passed = 0;
    if (!check(hsa_shut_down(), "shutdown runtime")) passed = 0;
    if (passed) puts("GPU allocations released and runtime owner connection closed");
    return passed ? 0 : 1;
}
