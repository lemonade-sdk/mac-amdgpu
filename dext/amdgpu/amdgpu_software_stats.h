#pragma once
#include <stdint.h>

namespace amdgpu::software_stats {
constexpr uint32_t kVersion = 1;
constexpr uint32_t kSelector = 61;
constexpr uint32_t kMinimumBuild = 193;
enum Engine : unsigned { SDMA0, SDMA1, GFX, AQL, EngineCount };
enum Direction : unsigned { HostToDevice, DeviceToHost, DeviceToDevice, HostToHost, Unknown, DirectionCount };
enum Flags : uint32_t { Available = 1, RuntimeReady = 2, Saturated = 4, QueueSampleIncomplete = 8 };
struct EngineSnapshot {
    uint64_t submitted, completed, failed, pending, retired;
    uint64_t pendingNs; // Union of software outstanding intervals, not GPU busy time.
    uint64_t bytes[DirectionCount]; // Completed known payload, never packet/ring overhead.
};
struct Snapshot {
    uint32_t version, size, flags, reserved;
    uint64_t generation, sampledAtNs, sessionStartNs;
    uint64_t participants, activeQueues, queuedPackets;
    uint64_t publishedPackets, consumedPackets, retiredPackets; // Consumption is not completion.
    uint64_t cpuUploadBytes, cpuReadbackBytes;
    EngineSnapshot engines[EngineCount];
};
static_assert(sizeof(Snapshot) == 456);

inline bool valid(const Snapshot &s) {
    if (s.version != kVersion || s.size != sizeof(s) || s.reserved ||
        (s.flags & ~(Available | RuntimeReady | Saturated | QueueSampleIncomplete)) ||
        !(s.flags & Available) || !s.generation || s.sampledAtNs < s.sessionStartNs ||
        s.consumedPackets > s.publishedPackets ||
        s.retiredPackets > s.publishedPackets - s.consumedPackets ||
        s.queuedPackets > s.publishedPackets - s.consumedPackets - s.retiredPackets) return false;
    for (const auto &e : s.engines) {
        if (e.completed > e.submitted || e.failed > e.submitted ||
            e.retired > e.submitted - e.completed ||
            e.pending > e.submitted - e.completed - e.retired ||
            e.pendingNs > s.sampledAtNs - s.sessionStartNs) return false;
    }
    return true;
}

// Single writer: the driver's lifecycle queue. Snapshot never polls a fence,
// opens PCI or sends a firmware message. Totals persist across session shutdown;
// only a new counter epoch (driver attachment) resets them.
struct Counters {
    Snapshot data{};
    uint64_t pendingSince[EngineCount]{};
    uint64_t vramBase = 0, vramBytes = 0, gartBase = 0, gartBytes = 0;

    void add(uint64_t &to, uint64_t amount) {
        if (amount > UINT64_MAX - to) { to = UINT64_MAX; data.flags |= Saturated; }
        else to += amount;
    }
    void reset(uint64_t now) {
        const uint64_t generation = data.generation == UINT64_MAX ? 1 : data.generation + 1;
        *this = {};
        data.version = kVersion; data.size = sizeof(Snapshot); data.flags = Available;
        data.generation = generation; data.sessionStartNs = now; data.sampledAtNs = now;
    }
    bool begin(Engine engine, uint64_t now, uint64_t count = 1) {
        if (engine >= EngineCount || !count || !data.generation || now < data.sessionStartNs) return false;
        auto &e = data.engines[engine];
        if (count > UINT64_MAX - e.submitted || count > UINT64_MAX - e.pending) {
            data.flags |= Saturated; return false;
        }
        if (!e.pending) pendingSince[engine] = now;
        e.submitted += count; e.pending += count;
        return true;
    }
    void complete(Engine engine, uint64_t now, uint64_t bytes = 0,
                  Direction direction = Unknown, uint64_t count = 1) {
        if (engine >= EngineCount || direction >= DirectionCount || !count) return;
        auto &e = data.engines[engine];
        if (count > e.pending || now < pendingSince[engine]) return;
        e.completed += count; e.pending -= count;
        if (!e.pending) add(e.pendingNs, now - pendingSince[engine]);
        add(e.bytes[direction], bytes);
    }
    void fail(Engine engine) {
        if (engine >= EngineCount) return;
        auto &e = data.engines[engine];
        // Failure does not prove work stopped. Leave pending until reset or
        // verified completion. Call once per accepted operation, not per wait.
        if (e.failed < e.submitted) ++e.failed;
    }
    void retireWork(uint64_t now) {
        // A verified reset ends observation, not necessarily execution at this
        // instant. Preserve totals and explicitly retire unobserved completion.
        for (unsigned i = 0; i < EngineCount; ++i) {
            auto &e = data.engines[i];
            if (!e.pending) continue;
            if (now >= pendingSince[i]) add(e.pendingNs, now - pendingSince[i]);
            add(e.retired, e.pending); e.pending = 0;
        }
        data.activeQueues = data.queuedPackets = 0;
        vramBase = vramBytes = gartBase = gartBytes = 0;
    }
    Snapshot snapshot(uint64_t now, bool ready) const {
        auto s = data;
        s.sampledAtNs = now < data.sessionStartNs ? data.sessionStartNs : now;
        if (ready) s.flags |= RuntimeReady;
        for (unsigned i = 0; i < EngineCount; ++i) {
            if (s.engines[i].pending && s.sampledAtNs >= pendingSince[i]) {
                const uint64_t extra = s.sampledAtNs - pendingSince[i];
                if (extra > UINT64_MAX - s.engines[i].pendingNs) s.flags |= Saturated;
                else s.engines[i].pendingNs += extra;
            }
        }
        return s;
    }
    static bool within(uint64_t address, uint64_t bytes, uint64_t base, uint64_t size) {
        return bytes && size && address >= base && address - base <= size && bytes <= size - (address - base);
    }
    Direction direction(uint64_t src, uint64_t dst, uint64_t bytes) const {
        const bool sv = within(src, bytes, vramBase, vramBytes), dv = within(dst, bytes, vramBase, vramBytes);
        const bool sh = within(src, bytes, gartBase, gartBytes), dh = within(dst, bytes, gartBase, gartBytes);
        return sv && dv ? DeviceToDevice : sh && dv ? HostToDevice :
               sv && dh ? DeviceToHost : sh && dh ? HostToHost : Unknown;
    }
};
// Construct only after successful publication. Later timeout/read failure
// counts once and leaves work pending; rejected preflight calls count zero.
struct PublishedWork {
    Counters *counters;
    Engine engine;
    bool tracked;
    PublishedWork(Counters *c, Engine e, uint64_t start)
        : counters(c), engine(e), tracked(c && c->begin(e, start)) {}
    ~PublishedWork() { if (tracked) counters->fail(engine); }
    void complete(uint64_t now, uint64_t bytes = 0, Direction direction = Unknown) {
        if (tracked) counters->complete(engine, now, bytes, direction);
        tracked = false;
    }
    void asynchronous() { tracked = false; }
};

struct QueueProgress {
    uint64_t handle = 0, published = 0, consumed = 0;
    void retire(Counters &c) {
        c.add(c.data.retiredPackets, published - consumed);
        *this = {};
    }
    bool observe(Counters &c, uint64_t id, uint64_t publish, uint64_t read) {
        if (!id || read > publish) return false;
        if (id != handle) { retire(c); handle = id; }
        if (publish < published || read < consumed) return false;
        c.add(c.data.publishedPackets, publish - published);
        c.add(c.data.consumedPackets, read - consumed);
        published = publish; consumed = read;
        return true;
    }
};
} // namespace amdgpu::software_stats
