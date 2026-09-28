#define LOAD(p) __scoped_atomic_load_n((p), __ATOMIC_ACQUIRE, __MEMORY_SCOPE_SYSTEM)
#define STORE(p, v) __scoped_atomic_store_n((p), (v), __ATOMIC_RELEASE, __MEMORY_SCOPE_SYSTEM)
#define RLOAD(p) __scoped_atomic_load_n((p), __ATOMIC_RELAXED, __MEMORY_SCOPE_SYSTEM)
#define RSTORE(p, v) __scoped_atomic_store_n((p), (v), __ATOMIC_RELAXED, __MEMORY_SCOPE_SYSTEM)
#define TURN 0
#define READY 16
#define ABORT 32
#define STATE 48
#define PAYLOAD 64
#define GO 80
#define PROGRESS 96
#define TIMES 128
#define TAG 0xabcdef0198765432UL

__attribute__((reqd_work_group_size(32, 1, 1)))
__kernel void hrx_touch(__global uint *data, uint sequence) {
  if (__builtin_amdgcn_workitem_id_x() == 0) data[0] = sequence;
}

// Each sequence gives payload ownership to one participant. No mixed RMW.
__attribute__((reqd_work_group_size(32, 1, 1)))
__kernel void latency_mailbox(__global ulong *data, ulong rounds, uint gpu_first) {
  if (__builtin_amdgcn_workitem_id_x() != 0) return;
  STORE(data + READY, 1UL);
  ulong attempts = 0;
  while (LOAD(data + GO) != 1UL) {
    if (++attempts >= 100000000UL || LOAD(data + ABORT)) goto cancelled;
  }
  for (ulong round = 1; round <= rounds; ++round) {
    ulong start = 0;
    if (gpu_first) {
      RSTORE(data + PAYLOAD, round);
      start = __builtin_amdgcn_s_sendmsg_rtnl(131);
      STORE(data + TURN, round * 2UL - 1UL);
    }
    attempts = 0;
    const ulong wanted = gpu_first ? round * 2UL : round * 2UL - 1UL;
    while (LOAD(data + TURN) != wanted) {
      if (++attempts >= 100000000UL || LOAD(data + ABORT)) goto cancelled;
    }
    if (gpu_first) {
      const ulong end = __builtin_amdgcn_s_sendmsg_rtnl(131);
      if (RLOAD(data + PAYLOAD) != (round ^ TAG)) goto mismatch;
      RSTORE(data + TIMES + round - 1UL, end - start);
    } else {
      if (RLOAD(data + PAYLOAD) != round) goto mismatch;
      RSTORE(data + PAYLOAD, round ^ TAG);
      STORE(data + TURN, round * 2UL);
    }
    RSTORE(data + PROGRESS, round);
  }
  STORE(data + STATE, 1UL);
  return;
mismatch:
  STORE(data + STATE, 3UL);
  return;
cancelled:
  STORE(data + STATE, 2UL);
}
