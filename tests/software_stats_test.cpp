#include "amdgpu_software_stats.h"
#include <cassert>
#include <cstdio>
using namespace amdgpu::software_stats;

int main() {
    Counters c;
    assert(!valid(c.snapshot(10, false)));
    c.reset(100);
    assert(valid(c.snapshot(100, false)) && !(c.snapshot(100, false).flags & RuntimeReady));
    assert(!c.begin(GFX, 99) && !c.begin(GFX, 100, 0));
    assert(c.begin(GFX, 110));
    assert(c.begin(GFX, 120));
    c.complete(GFX, 130);
    auto s = c.snapshot(140, true);
    assert(valid(s) && s.engines[GFX].pending == 1 && s.engines[GFX].pendingNs == 30);
    c.complete(GFX, 150);
    s = c.snapshot(180, true);
    assert(s.engines[GFX].pendingNs == 40); // Overlap is a union, not summed job durations.
    c.complete(GFX, 180); // Repeated observation must not complete a job twice.
    assert(c.data.engines[GFX].completed == 2);
    { PublishedWork work(&c, SDMA0, 200); work.complete(210, 4096, HostToDevice); }
    { PublishedWork work(&c, SDMA1, 220); } // Timeout: retain uncertain outstanding work.
    assert(c.data.engines[SDMA0].bytes[HostToDevice] == 4096);
    assert(c.data.engines[SDMA0].failed == 0 && c.data.engines[SDMA1].failed == 1);
    assert(c.snapshot(250, false).engines[SDMA1].pendingNs == 30);
    assert(c.data.engines[SDMA1].pendingNs == 0); // Sampling does not accumulate twice.
    c.complete(SDMA1, 260, 8192, DeviceToDevice);
    assert(c.data.engines[SDMA1].completed == 1 && c.data.engines[SDMA1].failed == 1);
    c.vramBase = 0x8000000000; c.vramBytes = 32ull << 30;
    c.gartBase = 0x100000000; c.gartBytes = 1ull << 30;
    assert(c.direction(c.gartBase, c.vramBase, 4096) == HostToDevice);
    assert(c.direction(c.vramBase, c.gartBase, 4096) == DeviceToHost);
    assert(c.direction(c.vramBase, c.vramBase + 4096, 4096) == DeviceToDevice);
    assert(c.direction(c.gartBase, c.gartBase + 4096, 4096) == HostToHost);
    assert(c.direction(UINT64_MAX - 4, c.vramBase, 4096) == Unknown);
    assert(c.direction(c.gartBase + c.gartBytes - 4, c.vramBase, 8) == Unknown);
    QueueProgress q;
    assert(q.observe(c, 1, 4, 1));
    assert(q.observe(c, 1, 4, 1));
    assert(c.data.publishedPackets == 4 && c.data.consumedPackets == 1);
    assert(!q.observe(c, 1, 4, 5));
    assert(!q.observe(c, 1, 3, 1));
    assert(!q.observe(c, 1, 4, 0));
    assert(q.observe(c, 1, 8, 3));
    assert(q.observe(c, 2, 1, 1)); // New queue, same slot: do not subtract old counters.
    assert(c.data.publishedPackets == 9 && c.data.consumedPackets == 4);
    assert(c.data.retiredPackets == 5);
    assert(c.data.engines[AQL].completed == 0); // Consumption never claims kernel completion.
    assert(c.begin(GFX, 270));
    const auto oldBytes = c.data.engines[SDMA0].bytes[HostToDevice];
    c.retireWork(280);
    assert(c.data.engines[GFX].retired == 1 && !c.data.engines[GFX].pending);
    assert(c.data.engines[GFX].completed == 2 && c.data.engines[SDMA0].bytes[HostToDevice] == oldBytes);
    auto oldGeneration = c.data.generation;
    c.reset(300);
    assert(c.data.generation != oldGeneration && !c.data.engines[SDMA1].pending);
    assert(c.data.publishedPackets == 0 && c.data.sessionStartNs == 300);
    assert(valid(c.snapshot(301, true)));
    c.data.cpuUploadBytes = UINT64_MAX - 1;
    c.add(c.data.cpuUploadBytes, 8);
    assert(c.data.cpuUploadBytes == UINT64_MAX && (c.data.flags & Saturated));
    puts("Software counters: publication, observed completion, retained failure, union intervals, direction, queue lifetime and reset pass");
}
