#pragma once
#include <stdint.h>
namespace amdgpu {
// Cached executable images each retain a BO alongside tensor/resource pools.
// Storage is lazy per client; observer connections allocate no entries.
constexpr uint32_t kBOEntriesPerPage = 64;
constexpr uint32_t kMaxBOPages = 64;
constexpr uint32_t kMaxClientBOs = kBOEntriesPerPage * kMaxBOPages;
}
