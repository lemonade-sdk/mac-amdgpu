#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <spawn.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern char **environ;
static hsa_agent_t gpu;
static hsa_amd_memory_pool_t pool;
static hsa_status_t agent(hsa_agent_t a, void *unused) {
    (void)unused;
    hsa_device_type_t type;
    hsa_status_t status = hsa_agent_get_info(a, HSA_AGENT_INFO_DEVICE, &type);
    if (!status && type == HSA_DEVICE_TYPE_GPU && !gpu.handle) gpu = a;
    return status;
}
static hsa_status_t memory_pool(hsa_amd_memory_pool_t p, void *unused) {
    (void)unused; pool = p; return HSA_STATUS_INFO_BREAK;
}
static int check(hsa_status_t status, const char *step) {
    if (!status) return 1;
    fprintf(stderr, "%s failed: HSA 0x%x\n", step, (unsigned)status); return 0;
}
static int transfer(int fd, void *bytes, size_t size, int sending) {
    char *p = bytes;
    while (size) {
        ssize_t n = sending ? write(fd, p, size) : read(fd, p, size);
        if (n <= 0) return 0;
        p += n; size -= (size_t)n;
    }
    return 1;
}
static void pattern(unsigned char *bytes) {
    for (size_t i = 0; i < 16384; ++i) bytes[i] = (unsigned char)(i * 67 + 11);
}
int main(int argc, char **argv) {
    int exporter = argc == 3 && !strcmp(argv[1], "--export");
    if (!exporter && (argc != 2 || strcmp(argv[1], "--run"))) {
        fprintf(stderr, "Usage: %s --run (requires driver 181)\n", argv[0]); return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!check(hsa_init(), "init") || !check(hsa_iterate_agents(agent, NULL), "enumerate") || !gpu.handle) return 1;
    hsa_amd_ipc_memory_t token = {{0}};
    unsigned char expected[16384], actual[16384]; pattern(expected);
    if (exporter) {
        int fd = atoi(argv[2]); void *allocation = NULL; char ready;
        hsa_status_t status = hsa_amd_agent_iterate_memory_pools(gpu, memory_pool, NULL);
        if (status != HSA_STATUS_INFO_BREAK || !pool.handle) return 1;
        if (!check(hsa_amd_memory_pool_allocate(pool, sizeof(expected), 0, &allocation), "allocate exporter buffer") ||
            !check(hsa_memory_copy(allocation, expected, sizeof(expected)), "exporter upload") ||
            !check(hsa_amd_ipc_memory_create(allocation, sizeof(expected), &token), "export GPU buffer") ||
            !transfer(fd, &token, sizeof(token), 1) || !transfer(fd, &ready, 1, 0)) return 1;
        puts("Exporter exiting without HSA cleanup; importer must retain the VRAM allocation");
        _exit(0);
    }
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return 1;
    struct timeval timeout = {15, 0};
    setsockopt(pair[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(pair[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    int noSignal = 1;
    setsockopt(pair[0], SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal));
    setsockopt(pair[1], SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal));
    char descriptor[32]; snprintf(descriptor, sizeof(descriptor), "%d", pair[1]);
    char *args[] = {argv[0], "--export", descriptor, NULL};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions); posix_spawn_file_actions_addclose(&actions, pair[0]);
    pid_t pid;
    int spawned = posix_spawn(&pid, argv[0], &actions, NULL, args, environ);
    posix_spawn_file_actions_destroy(&actions); close(pair[1]);
    if (spawned) { close(pair[0]); hsa_shut_down(); return 1; }
    void *imported = NULL, *duplicate = NULL;
    int passed = 0, waited = 0, status = 0;
    if (!transfer(pair[0], &token, sizeof(token), 0)) goto cleanup;
    if (!check(hsa_amd_ipc_memory_attach(&token, sizeof(actual), 1, &gpu, &imported), "import GPU buffer")) goto cleanup;
    if (!check(hsa_memory_copy(actual, imported, sizeof(actual)), "read shared GPU buffer") || memcmp(actual, expected, sizeof(actual))) goto cleanup;
    if (!check(hsa_amd_ipc_memory_attach(&token, sizeof(actual), 1, &gpu, &duplicate), "repeat attachment")) goto cleanup;
    char ready = 1;
    if (!transfer(pair[0], &ready, 1, 1)) goto cleanup;
    if (waitpid(pid, &status, 0) != pid) goto cleanup;
    waited = 1;
    if (!WIFEXITED(status) || WEXITSTATUS(status)) goto cleanup;
    if (!check(hsa_amd_ipc_memory_detach(duplicate), "detach duplicate")) goto cleanup;
    duplicate = NULL;
    if (!check(hsa_memory_copy(actual, imported, sizeof(actual)), "read after exporter exit") || memcmp(actual, expected, sizeof(actual))) goto cleanup;
    if (!check(hsa_amd_memory_fill(imported, 0x6b6b6b6b, sizeof(actual) / 4), "write imported GPU buffer") ||
        !check(hsa_memory_copy(actual, imported, sizeof(actual)), "read imported writes")) goto cleanup;
    for (size_t i = 0; i < sizeof(actual); ++i) if (actual[i] != 0x6b) goto cleanup;
    puts("PASS: all 16384 bytes verified before and after exporter process exit; imported writes and duplicate attachment verified");
    passed = 1;
cleanup:
    close(pair[0]);
    if (!waited) waitpid(pid, &status, 0);
    if (duplicate && !check(hsa_amd_ipc_memory_detach(duplicate), "detach duplicate")) passed = 0;
    if (imported && !check(hsa_amd_ipc_memory_detach(imported), "detach import")) passed = 0;
    if (!check(hsa_shut_down(), "shutdown")) passed = 0;
    return passed ? 0 : 1;
}
