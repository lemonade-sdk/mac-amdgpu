#include <hsa/hsa.h>
#include <hsa/hsa_ven_amd_loader.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static hsa_agent_t gpu;
static hsa_status_t agent(hsa_agent_t handle, void *unused) {
    (void)unused;
    hsa_device_type_t type;
    hsa_status_t status = hsa_agent_get_info(handle, HSA_AGENT_INFO_DEVICE, &type);
    if (status == HSA_STATUS_SUCCESS && type == HSA_DEVICE_TYPE_GPU && !gpu.handle) gpu = handle;
    return status;
}
static int check(hsa_status_t status, const char *step) {
    if (status == HSA_STATUS_SUCCESS) return 1;
    fprintf(stderr, "%s failed: HSA status 0x%x\n", step, (unsigned)status); return 0;
}
int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "Usage: %s linked.hsaco kernel-symbol\n", argv[0]); return 2; }
    FILE *file = fopen(argv[1], "rb");
    if (!file) { perror("open code object"); return 1; }
    if (fseek(file, 0, SEEK_END)) { fclose(file); return 1; }
    long size = ftell(file);
    if (size <= 0 || size > 256 * 1024 * 1024 || fseek(file, 0, SEEK_SET)) { fclose(file); return 1; }
    void *bytes = malloc((size_t)size);
    if (!bytes) { fclose(file); return 1; }
    size_t read = fread(bytes, 1, (size_t)size, file);
    fclose(file);
    if (read != (size_t)size || !check(hsa_init(), "runtime init")) { free(bytes); return 1; }
    hsa_ven_amd_loader_1_03_pfn_t loader = {0};
    hsa_loaded_code_object_t loaded = {0};
    hsa_code_object_reader_t reader = {0};
    hsa_executable_t executable = {0};
    hsa_executable_symbol_t symbol = {0};
    int passed = 0;
    uint64_t address = 0;
    uint32_t validation = ~0u, kernarg = 0;
    if (!check(hsa_system_get_major_extension_table(HSA_EXTENSION_AMD_LOADER, 1, sizeof(loader), &loader), "AMD loader extension")) goto cleanup;
    if (!check(hsa_iterate_agents(agent, NULL), "enumerate") || !gpu.handle) goto cleanup;
    if (!check(hsa_code_object_reader_create_from_memory(bytes, read, &reader), "create reader")) goto cleanup;
    free(bytes); bytes = NULL;
    if (!check(hsa_executable_create_alt(HSA_PROFILE_BASE, HSA_DEFAULT_FLOAT_ROUNDING_MODE_DEFAULT,
                                         NULL, &executable), "create executable")) goto cleanup;
    if (!check(hsa_executable_load_agent_code_object(executable, gpu, reader, NULL, &loaded), "load GPU executable")) goto cleanup;
    if (!check(hsa_code_object_reader_destroy(reader), "release temporary reader")) goto cleanup;
    reader.handle = 0;
    if (!check(hsa_executable_freeze(executable, NULL), "freeze executable")) goto cleanup;
    if (!check(hsa_executable_validate_alt(executable, NULL, &validation), "validate executable") || validation) goto cleanup;
    if (!check(hsa_executable_get_symbol_by_name(executable, argv[2], &gpu, &symbol), "resolve kernel")) goto cleanup;
    if (!check(hsa_executable_symbol_get_info(symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT, &address), "kernel descriptor") || !address) goto cleanup;
    if (!check(hsa_executable_symbol_get_info(symbol, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_KERNARG_SEGMENT_SIZE, &kernarg), "kernel arguments")) goto cleanup;
    const void *descriptor = NULL;
    hsa_executable_t owner = {0};
    uint64_t base = 0, loaded_size = 0;
    if (!check(loader.hsa_ven_amd_loader_query_host_address((const void *)(uintptr_t)address, &descriptor), "translate kernel descriptor")) goto cleanup;
    if (!check(loader.hsa_ven_amd_loader_query_executable((const void *)(uintptr_t)address, &owner), "descriptor executable") || owner.handle != executable.handle) goto cleanup;
    if (!check(loader.hsa_ven_amd_loader_loaded_code_object_get_info(loaded, HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_LOAD_BASE, &base), "load base")) goto cleanup;
    if (!check(loader.hsa_ven_amd_loader_loaded_code_object_get_info(loaded, HSA_VEN_AMD_LOADER_LOADED_CODE_OBJECT_INFO_LOAD_SIZE, &loaded_size), "load size")) goto cleanup;
    uint32_t descriptor_kernarg = 0;
    memcpy(&descriptor_kernarg, (const char *)descriptor + 8, sizeof(descriptor_kernarg));
    if (descriptor_kernarg != kernarg || address < base || address - base >= loaded_size) goto cleanup;
    printf("AMD loader: descriptor host translation verified after reader destruction; base=0x%llx size=%llu\n", (unsigned long long)base, (unsigned long long)loaded_size);
    printf("PASS: linked GPU executable loaded and frozen; %s descriptor=0x%llx kernarg=%u bytes\n",
           argv[2], (unsigned long long)address, kernarg);
    puts("This verifies loading and symbol resolution; it does not dispatch the kernel.");
    passed = 1;
cleanup:
    if (executable.handle && !check(hsa_executable_destroy(executable), "destroy executable")) passed = 0;
    if (reader.handle && !check(hsa_code_object_reader_destroy(reader), "destroy reader")) passed = 0;
    if (!check(hsa_shut_down(), "shutdown")) passed = 0;
    free(bytes);
    return passed ? 0 : 1;
}
