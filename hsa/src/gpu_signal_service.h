#pragma once
#include "transport.h"
#include <chrono>
#include <memory>

namespace mac_hsa {
enum class SignalServiceResult { Success, Unavailable, Failed };
// Optional CPU HSA signal backend. It borrows the arena; shutdown must succeed
// before the caller may free that arena. It never retries a published request.
class GPUSignalService {
    struct State;
    std::unique_ptr<State> state_;
public:
    GPUSignalService(std::shared_ptr<Connection> connection,const SharedBuffer &arena,
        std::chrono::milliseconds idle=std::chrono::milliseconds(50));
    ~GPUSignalService();
    SignalServiceResult execute(unsigned slot,unsigned operation,int64_t value,int64_t compare,int64_t &old);
    bool reclaim(); // releases an internal queue before a public queue allocation
    bool shutdown(); // stops worker, verifies queue unmap; false retains all backing
    bool healthy() const;
};
}
