#include "runtime_state.h"
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>

namespace mac_hsa::detail {
namespace {
constexpr uint64_t magic = 0x314749534148434dull; // versioned local macOS ABI
struct Token { uint64_t magic, nonce[2], reserved; };
struct alignas(64) SharedPage {
    Token token;
    uint64_t retired;
    alignas(64) mac_hsa::SignalABI signal;
};
static_assert(offsetof(SharedPage, signal) == 64 && sizeof(Token) == sizeof(hsa_amd_ipc_signal_t));
std::string tokenPath(const Token &token) {
    char directory[4096];
    const size_t needed = confstr(_CS_DARWIN_USER_TEMP_DIR, directory, sizeof(directory));
    if (!needed || needed > sizeof(directory)) return {};
    char name[64];
    std::snprintf(name, sizeof(name), "mac-hsa-signal-%016llx%016llx", (unsigned long long)token.nonce[0], (unsigned long long)token.nonce[1]);
    return std::string(directory) + name;
}
struct SharedSignal {
    int fd = -1;
    SharedPage *page = nullptr;
    std::string path;
    bool attached = false, created = false;
    ~SharedSignal() {
        if (fd >= 0) {
            if (attached) {
                // Kernel-held shared locks disappear on process death. The last
                // surviving attachment retires the name without a leaked manual
                // refcount when another process exits abruptly.
                flock(fd, LOCK_UN);
                if (!flock(fd, LOCK_EX | LOCK_NB)) {
                    std::atomic_ref<uint64_t>(page->retired).store(1, std::memory_order_release);
                    unlink(path.c_str());
                }
            } else if (created) unlink(path.c_str());
            if (page) munmap(page, hostPageSize());
            close(fd);
        }
    }
};
hsa_status_t publish(const std::shared_ptr<SharedSignal> &storage, hsa_signal_t *out) {
    auto signal = std::make_shared<mac_hsa::Signal>();
    signal->sharedStorage = storage; signal->sharedABI = &storage->page->signal;
    std::memcpy(signal->ipcToken, &storage->page->token, sizeof(Token));
    const auto address = uint64_t(uintptr_t(signal->address()));
    if (!signals.emplace(address, std::move(signal)).second) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    out->handle = address; return HSA_STATUS_SUCCESS;
}
}
hsa_status_t createIPCSignal(hsa_signal_value_t initial, uint32_t count,
    const hsa_agent_t *consumers, hsa_signal_t *out) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!out || (count && !consumers)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    *out = {};
    if (!count) for (const auto &agent : agents) if (agent.connection) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    for (uint32_t i = 0; i < count; ++i) {
        const auto agent = findAgent(consumers[i]);
        if (!agent) return HSA_STATUS_ERROR_INVALID_AGENT;
        for (uint32_t j = 0; j < i; ++j) if (consumers[i].handle == consumers[j].handle) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        if (agent->connection) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    }
    try {
        auto storage = std::make_shared<SharedSignal>();
        Token token{magic, {}, 0}; arc4random_buf(token.nonce, sizeof(token.nonce));
        storage->path = tokenPath(token);
        if (storage->path.empty()) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        storage->fd = open(storage->path.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (storage->fd < 0) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        storage->created = true;
        if (flock(storage->fd, LOCK_EX) || ftruncate(storage->fd, off_t(hostPageSize()))) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        auto page = mmap(nullptr, hostPageSize(), PROT_READ | PROT_WRITE, MAP_SHARED, storage->fd, 0);
        if (page == MAP_FAILED) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        storage->page = new (page) SharedPage{};
        storage->page->token = token; storage->page->signal.value = initial;
        if (flock(storage->fd, LOCK_SH)) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        storage->attached = true;
        return publish(storage, out);
    } catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
}
}
using namespace mac_hsa::detail;
extern "C" {
HSA_API_EXPORT hsa_status_t hsa_amd_ipc_signal_create(hsa_signal_t handle, hsa_amd_ipc_signal_t *out) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!out) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    *out = {};
    const auto found = signals.find(handle.handle);
    if (found == signals.end() || !found->second->sharedABI) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    std::memcpy(out, found->second->ipcToken, sizeof(*out)); return HSA_STATUS_SUCCESS;
}
HSA_API_EXPORT hsa_status_t hsa_amd_ipc_signal_attach(const hsa_amd_ipc_signal_t *handle, hsa_signal_t *out) {
    std::lock_guard lock(runtimeMutex);
    if (!references) return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (!out) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    *out = {};
    if (!handle) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    Token token; std::memcpy(&token, handle, sizeof(token));
    if (token.magic != magic || token.reserved || (!token.nonce[0] && !token.nonce[1])) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    try {
        auto storage = std::make_shared<SharedSignal>();
        storage->path = tokenPath(token);
        if (storage->path.empty()) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        storage->fd = open(storage->path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (storage->fd < 0) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        struct stat info{};
        if (fstat(storage->fd, &info) || !S_ISREG(info.st_mode) || info.st_uid != getuid() ||
            info.st_size != off_t(hostPageSize()) || (info.st_mode & 077))
            return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        // An orphan file is not a live signal. Require another attachment's
        // shared lock; never revive a token after all participants have died.
        if (!flock(storage->fd, LOCK_EX | LOCK_NB)) {
            unlink(storage->path.c_str()); return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        }
        if (flock(storage->fd, LOCK_SH | LOCK_NB)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        auto page = mmap(nullptr, hostPageSize(), PROT_READ | PROT_WRITE, MAP_SHARED, storage->fd, 0);
        if (page == MAP_FAILED) return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
        storage->page = static_cast<SharedPage *>(page);
        if (std::memcmp(&storage->page->token, &token, sizeof(token)) ||
            std::atomic_ref<uint64_t>(storage->page->retired).load(std::memory_order_acquire)) return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        storage->attached = true;
        return publish(storage, out);
    } catch (const std::bad_alloc &) { return HSA_STATUS_ERROR_OUT_OF_RESOURCES; }
}
}
