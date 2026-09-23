#include "runtime_state.h"
#include <hsa/amd_hsa_queue.h>
#include <cassert>
#include <cstdio>
#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
extern char **environ;

static unsigned imports = 0, frees = 0;
namespace mac_hsa {
struct TestConnection final : Connection {
    bool supportsBuffers() const override { return true; }
    hsa_status_t read(DeviceSnapshot &s) override { s = {123, 181, 15, 1, 1, 12, 0, 1}; return HSA_STATUS_SUCCESS; }
    hsa_status_t importBuffer(const BufferToken &token, DeviceBuffer &buffer) override {
        if (token.registryID != 123 || token.token[0] != 456 || token.token[1] != 789 || token.size != 16384)
            return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        buffer = {++imports, 0x8010000000, 16384}; return HSA_STATUS_SUCCESS;
    }
    hsa_status_t exportBuffer(const DeviceBuffer &, BufferToken &token) override {
        token = {123, {456, 789}, 16384}; return HSA_STATUS_SUCCESS;
    }
    hsa_status_t freeBuffer(const DeviceBuffer &) override {
        uint64_t now; hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP, &now); ++frees; return HSA_STATUS_SUCCESS;
    }
};
hsa_status_t discover(std::vector<std::shared_ptr<Connection>> &connections) {
    connections.push_back(std::make_shared<TestConnection>()); return HSA_STATUS_SUCCESS;
}
}
static hsa_agent_t cpu, gpu;
static hsa_region_t region;
static void initialize() {
    assert(hsa_init() == 0);
    assert(hsa_iterate_agents([](hsa_agent_t a, void *) {
        hsa_device_type_t type; assert(hsa_agent_get_info(a, HSA_AGENT_INFO_DEVICE, &type) == 0);
        (type == HSA_DEVICE_TYPE_CPU ? cpu : gpu) = a; return HSA_STATUS_SUCCESS;
    }, nullptr) == 0);
    assert(hsa_agent_iterate_regions(cpu, [](hsa_region_t r, void *) { region = r; return HSA_STATUS_SUCCESS; }, nullptr) == 0);
}
static void wait(hsa_signal_t signal, int64_t value) {
    assert(hsa_signal_wait_scacquire(signal, HSA_SIGNAL_CONDITION_EQ, value, 10000000000ull, HSA_WAIT_STATE_BLOCKED) == value);
}
static std::string encode(const hsa_amd_ipc_signal_t &token) {
    char output[65];
    for (unsigned i = 0; i < 8; ++i) std::snprintf(output + i * 8, 9, "%08x", token.handle[i]);
    return output;
}
static pid_t child(const char *self, const char *mode, const std::string &token) {
    char *args[]{const_cast<char *>(self), const_cast<char *>(mode), const_cast<char *>(token.c_str()), nullptr};
    pid_t pid; assert(posix_spawn(&pid, self, nullptr, nullptr, args, environ) == 0); return pid;
}
int main(int argc, char **argv) {
    initialize();
    if (argc == 3 && !std::strcmp(argv[1], "--orphan")) {
        hsa_signal_t signal{}; hsa_amd_ipc_signal_t token{};
        assert(hsa_amd_signal_create(1, 1, &cpu, HSA_AMD_SIGNAL_IPC, &signal) == 0);
        assert(hsa_amd_ipc_signal_create(signal, &token) == 0);
        assert(write(std::atoi(argv[2]), &token, sizeof(token)) == sizeof(token));
        for (;;) pause();
    }
    if (argc == 3) {
        hsa_amd_ipc_signal_t token{};
        assert(std::strlen(argv[2]) == 64);
        for (unsigned i = 0; i < 8; ++i) assert(std::sscanf(argv[2] + i * 8, "%8x", &token.handle[i]) == 1);
        hsa_signal_t signal{};
        assert(hsa_amd_ipc_signal_attach(&token, &signal) == 0);
        hsa_signal_store_screlease(signal, 2);
        if (!std::strcmp(argv[1], "--killed")) for (;;) pause();
        wait(signal, 3);
        hsa_signal_add_scacq_screl(signal, 1);
        assert(hsa_signal_destroy(signal) == 0 && hsa_shut_down() == 0);
        return 0;
    }
    hsa_signal_t signal{}, mirror{}, later{};
    hsa_amd_ipc_signal_t token{};
    assert(hsa_amd_signal_create(1, 1, &cpu, HSA_AMD_SIGNAL_IPC, &signal) == 0);
    assert(hsa_amd_ipc_signal_create(signal, &token) == 0);
    assert(hsa_amd_ipc_signal_attach(&token, &mirror) == 0 && mirror.handle != signal.handle);
    auto text = encode(token);
    pid_t pid = child(argv[0], "--child", text);
    wait(signal, 2);
    assert(hsa_signal_destroy(signal) == 0);
    assert(hsa_amd_ipc_signal_attach(&token, &later) == 0);
    hsa_signal_store_screlease(mirror, 3); wait(later, 4);
    int status; assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(hsa_signal_destroy(later) == 0);
    hsa_signal_store_relaxed(mirror, 1);
    pid = child(argv[0], "--killed", text); wait(mirror, 2);
    assert(kill(pid, SIGKILL) == 0);
    assert(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status));
    assert(hsa_signal_destroy(mirror) == 0);
    assert(hsa_amd_ipc_signal_attach(&token, &later) == HSA_STATUS_ERROR_INVALID_ARGUMENT && !later.handle);
    int channel[2]; assert(pipe(channel) == 0);
    pid = child(argv[0], "--orphan", std::to_string(channel[1])); close(channel[1]);
    assert(read(channel[0], &token, sizeof(token)) == sizeof(token)); close(channel[0]);
    assert(kill(pid, SIGKILL) == 0 && waitpid(pid, &status, 0) == pid);
    assert(hsa_amd_ipc_signal_attach(&token, &later) == HSA_STATUS_ERROR_INVALID_ARGUMENT && !later.handle);

    // Imported device memory has balanced attach/detach references. Callback
    // release can query HSA without deadlocking the runtime mutex.
    mac_hsa::BufferToken bufferToken{123, {456, 789}, 16384};
    hsa_amd_ipc_memory_t memoryToken; std::memcpy(&memoryToken, &bufferToken, sizeof(memoryToken));
    void *first = nullptr, *second = nullptr;
    assert(hsa_amd_ipc_memory_attach(&memoryToken, 16384, 1, &gpu, &first) == 0);
    assert(hsa_amd_ipc_memory_attach(&memoryToken, 16384, 1, &gpu, &second) == 0 && first == second && imports == 1);
    assert(hsa_memory_free(first) == HSA_STATUS_ERROR_INVALID_ALLOCATION);
    hsa_amd_pointer_info_t info{}; info.size = sizeof(info);
    assert(hsa_amd_pointer_info(first, &info, nullptr, nullptr, nullptr) == 0 && info.type == HSA_EXT_POINTER_TYPE_IPC);
    hsa_amd_ipc_memory_t reexport{};
    assert(hsa_amd_ipc_memory_create(first, 16384, &reexport) == 0 && !std::memcmp(&reexport, &memoryToken, sizeof(reexport)));
    assert(hsa_amd_ipc_memory_detach(first) == 0 && frees == 0);
    assert(hsa_amd_ipc_memory_detach(second) == 0 && frees == 1);
    assert(hsa_amd_ipc_memory_detach(second) == HSA_STATUS_ERROR_INVALID_ARGUMENT);

    assert(hsa_signal_create(7, 1, &cpu, &signal) == 0);
    assert(hsa_amd_ipc_signal_create(signal, &token) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
    hsa_queue_t *queue = nullptr;
    assert(hsa_soft_queue_create(region, 64, HSA_QUEUE_TYPE_MULTI, HSA_QUEUE_FEATURE_AGENT_DISPATCH, signal, &queue) == 0);
    auto &abi = *reinterpret_cast<amd_queue_t *>(queue);
    assert(hsa_amd_profiling_set_profiler_enabled(queue, 1) == 0 && (abi.queue_properties & AMD_QUEUE_PROPERTIES_ENABLE_PROFILING));
    assert(hsa_amd_profiling_set_profiler_enabled(queue, 0) == 0 && !(abi.queue_properties & AMD_QUEUE_PROPERTIES_ENABLE_PROFILING));
    assert(hsa_amd_queue_set_priority(queue, HSA_AMD_QUEUE_PRIORITY_NORMAL) == HSA_STATUS_ERROR_INVALID_QUEUE);
    assert(hsa_amd_queue_cu_set_mask(queue, 0, nullptr) == HSA_STATUS_ERROR_INVALID_QUEUE);
    uint64_t untouched = 99;
    assert(hsa_amd_queue_get_info(queue, HSA_AMD_QUEUE_INFO_DOORBELL_ID, &untouched) == HSA_STATUS_ERROR_INVALID_QUEUE && untouched == 99);
    assert(hsa_queue_destroy(queue) == 0);
    assert(hsa_amd_profiling_set_profiler_enabled(queue, 1) == HSA_STATUS_ERROR_INVALID_QUEUE);
    int calls = 0;
    auto callback = [](const hsa_amd_event_t *event, void *data) {
        uint64_t now; assert(hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP, &now) == 0);
        assert(event->event_type == HSA_AMD_GPU_MEMORY_ERROR_EVENT);
        ++*static_cast<int *>(data); return HSA_STATUS_INFO_BREAK;
    };
    assert(hsa_amd_register_system_event_handler(callback, &calls) == 0);
    assert(hsa_amd_register_system_event_handler(callback, &calls) == HSA_STATUS_ERROR);
    hsa_amd_event_t event{}; event.event_type = HSA_AMD_GPU_MEMORY_ERROR_EVENT;
    assert(mac_hsa::detail::deliverSystemEvent(event) == HSA_STATUS_INFO_BREAK && calls == 1);
    void *host = nullptr; assert(hsa_memory_allocate(region, 16384, &host) == 0);
    hsa_amd_svm_attribute_pair_t attribute{HSA_AMD_SVM_ATTRIB_READ_MOSTLY, 42};
    assert(hsa_amd_svm_attributes_set(host, 16, &attribute, 1) == HSA_STATUS_ERROR);
    assert(hsa_amd_svm_attributes_get(host, 16, &attribute, 1) == HSA_STATUS_ERROR && attribute.value == 42);
    assert(hsa_amd_svm_prefetch_async(host, 16, gpu, 0, nullptr, signal) == HSA_STATUS_ERROR && hsa_signal_load_relaxed(signal) == 7);
    bool supported = true;
    assert(hsa_system_get_info(HSA_AMD_SYSTEM_INFO_SVM_SUPPORTED, &supported) == 0 && !supported);
    int fd = open("/dev/null", O_RDWR); assert(fd >= 0);
    int outputFD = fd; uint64_t offset = 123;
    assert(hsa_amd_portable_export_dmabuf(host, 16, &outputFD, &offset) == HSA_STATUS_ERROR_INVALID_AGENT && outputFD == fd && offset == 123);
    assert(hsa_amd_portable_close_dmabuf(fd) == HSA_STATUS_ERROR_INVALID_ARGUMENT && fcntl(fd, F_GETFD) >= 0);
    size_t size = 1; void *mapped = host;
    assert(hsa_amd_interop_map_buffer(1, &gpu, fd, 0, &size, &mapped, nullptr, nullptr) == HSA_STATUS_ERROR_INVALID_ARGUMENT && !size && !mapped);
    assert(hsa_amd_interop_unmap_buffer(host) == HSA_STATUS_ERROR_INVALID_ARGUMENT);
    close(fd);
    assert(hsa_memory_free(host) == 0 && hsa_shut_down() == 0);
    assert(hsa_init() == 0 && hsa_amd_register_system_event_handler(callback, &calls) == 0 && hsa_shut_down() == 0);
    puts("HSA extensions: cross-process signals, abrupt peer death, GPU import references, queue controls, events and unsupported-platform contracts pass");
}
