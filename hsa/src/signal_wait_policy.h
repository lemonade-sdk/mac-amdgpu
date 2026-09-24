#pragma once

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace mac_hsa {

// Blocked waits still poll because GPU DMA stores do not notify a host
// condition variable. Keep the established default; this bounded override
// measures wake latency independently of shader, mapping and batching changes.
inline uint64_t blockedSignalPollNs(const char *setting) {
    constexpr uint64_t fallback=1000000;
    if (!setting) return fallback;
    const std::string_view text(setting);
    uint32_t micros=0;
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),micros);
    if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() ||
        micros<10 || micros>1000) return fallback;
    return uint64_t(micros)*1000;
}

inline uint64_t blockedSignalPollNs() {
    static const uint64_t interval=blockedSignalPollNs(std::getenv("MAC_HSA_BLOCKED_POLL_US"));
    return interval;
}

} // namespace mac_hsa
