// One owner at a time. Payload stores/loads are relaxed; ownership release/acquire
// publishes the complete payload in each direction without a contended RMW.
#define LOAD(p) __scoped_atomic_load_n((p),__ATOMIC_ACQUIRE,__MEMORY_SCOPE_SYSTEM)
#define STORE(p,v) __scoped_atomic_store_n((p),(v),__ATOMIC_RELEASE,__MEMORY_SCOPE_SYSTEM)
#define RELAXED_LOAD(p) __scoped_atomic_load_n((p),__ATOMIC_RELAXED,__MEMORY_SCOPE_SYSTEM)
#define RELAXED_STORE(p,v) __scoped_atomic_store_n((p),(v),__ATOMIC_RELAXED,__MEMORY_SCOPE_SYSTEM)
#define TURN 0
#define READY 16
#define ABORT 32
#define STATE 48
#define PROGRESS 64
#define ERRORS 80
#define FIRST_ROUND 96
#define FIRST_WORD 97
#define FIRST_EXPECTED 98
#define FIRST_OBSERVED 99
#define PAYLOAD 128
#define PAYLOAD_WORDS 64
#define CPU_TAG 0x13579bdf2468ace0UL
#define GPU_TAG 0xfedcba9876543210UL
#define MIX 0x9e3779b97f4a7c15UL

__attribute__((reqd_work_group_size(32,1,1)))
__kernel void ownership_ping_pong(__global ulong *data, ulong rounds, ulong max_attempts) {
    if (__builtin_amdgcn_workitem_id_x()) return;
    ulong completed=0;
    STORE(data+READY,1UL);
    for (ulong round=0;round<rounds;++round) {
        const ulong cpu_turn=round*2UL+1UL;
        ulong attempts=0;
        while (LOAD(data+TURN)!=cpu_turn) {
            if (++attempts>=max_attempts || LOAD(data+ABORT)) goto cancelled;
        }
        if (LOAD(data+ABORT)) goto cancelled;
        for (ulong word=0;word<PAYLOAD_WORDS;++word) {
            const ulong expected=CPU_TAG ^ ((round+1UL)*MIX) ^ (word*0x0101010101010101UL);
            const ulong observed=RELAXED_LOAD(data+PAYLOAD+word);
            if (observed!=expected) {
                RELAXED_STORE(data+FIRST_ROUND,round+1UL);
                RELAXED_STORE(data+FIRST_WORD,word);
                RELAXED_STORE(data+FIRST_EXPECTED,expected);
                RELAXED_STORE(data+FIRST_OBSERVED,observed);
                RELAXED_STORE(data+ERRORS,1UL);
                STORE(data+STATE,3UL);return;
            }
        }
        for (ulong word=0;word<PAYLOAD_WORDS;++word)
            RELAXED_STORE(data+PAYLOAD+word,GPU_TAG ^ ((round+1UL)*MIX) ^ (word*0x0101010101010101UL));
        completed=round+1UL;
        RELAXED_STORE(data+PROGRESS,completed);
        STORE(data+TURN,cpu_turn+1UL);
    }
    STORE(data+STATE,1UL);return;
cancelled:
    STORE(data+PROGRESS,completed);
    STORE(data+STATE,2UL);
}
