// Native system-scope atomics: no HSA signal API mediates the shared words.
// Each control cell starts a separate 128-byte region; the protected payload
// has a counter and its inverse to expose broken mutual exclusion/publication.
#define LOAD(p) __scoped_atomic_load_n((p),__ATOMIC_ACQUIRE,__MEMORY_SCOPE_SYSTEM)
#define LOAD_SC(p) __scoped_atomic_load_n((p),__ATOMIC_SEQ_CST,__MEMORY_SCOPE_SYSTEM)
#define STORE_SC(p,v) __scoped_atomic_store_n((p),(v),__ATOMIC_SEQ_CST,__MEMORY_SCOPE_SYSTEM)
#define STORE(p,v) __scoped_atomic_store_n((p),(v),__ATOMIC_RELEASE,__MEMORY_SCOPE_SYSTEM)
#define CAS(p,e,v) __scoped_atomic_compare_exchange_n((p),(e),(v),0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE,__MEMORY_SCOPE_SYSTEM)
#define COUNTER 0
#define LOCK 16
#define PAYLOAD 32
#define INVERSE 33
#define CPU_INSIDE 48
#define GPU_INSIDE 64
#define GPU_ERRORS 96
#define READY 112
#define GO 128
#define CPU_PROGRESS 144
#define GPU_PROGRESS 160
#define ABORT 176
#define STATE 192
#define GPU_ATTEMPTS 208
#define GPU_OVERLAP 224
#define CPU_OLD_SUM 240
#define GPU_OLD_SUM 256
#define HANDOFF_RESULT 272
#define GPU_STARTED 288
#define GPU_FINISHED 304
#define MAGIC 0x6d3f0a519c27e8b4UL

__attribute__((reqd_work_group_size(32,1,1)))
__kernel void atomic_contention(__global ulong *data, ulong iterations,
                               ulong max_attempts, unsigned mode, ulong cpu_iterations,
                               ulong cpu_overlap_floor) {
    if (__builtin_amdgcn_workitem_id_x()) return;
    STORE(data+READY,1UL);
    ulong attempts=0, completed=0, errors=0, overlap=0, old_sum=0;
    while (!LOAD(data+GO)) {
        if (++attempts>=max_attempts || LOAD(data+ABORT)) {
            STORE(data+STATE,2UL);return;
        }
    }
    attempts=0;
    STORE(data+GPU_STARTED,1UL);
    for (ulong i=0;i<iterations;++i) {
        if (!(i&255UL) && LOAD(data+ABORT)) break;
        if (!mode || mode==2) {
            if (mode==2)
                old_sum+=__scoped_atomic_fetch_add(data+COUNTER,1UL,__ATOMIC_SEQ_CST,__MEMORY_SCOPE_SYSTEM);
            else __scoped_atomic_fetch_add(data+COUNTER,1UL,__ATOMIC_SEQ_CST,__MEMORY_SCOPE_SYSTEM);
        } else {
            ulong expected=0;
            while (!CAS(data+LOCK,&expected,2UL)) {
                expected=0;
                if (++attempts>=max_attempts || LOAD(data+ABORT)) goto finished;
            }
            STORE_SC(data+GPU_INSIDE,1UL);
            if (LOAD_SC(data+CPU_INSIDE)) ++errors;
            const ulong previous=LOAD(data+PAYLOAD);
            if (LOAD(data+INVERSE)!=(previous^MAGIC)) ++errors;
            // Stores remain atomic even if the tested lock is broken, so a
            // failure cannot turn the instrumentation into torn plain writes.
            STORE(data+PAYLOAD,previous+1UL);
            STORE(data+INVERSE,(previous+1UL)^MAGIC);
            if (LOAD_SC(data+CPU_INSIDE)) ++errors;
            STORE_SC(data+GPU_INSIDE,0UL);
            expected=2;
            if (!CAS(data+LOCK,&expected,0UL)) {++errors;goto finished;}
        }
        completed=i+1;
        if (!(i&255UL)) {
            const ulong cpu=LOAD(data+CPU_PROGRESS);
            if (cpu>cpu_overlap_floor && cpu<cpu_iterations) ++overlap;
            STORE(data+GPU_PROGRESS,completed);
        }
    }
finished:
    STORE(data+GPU_PROGRESS,completed);
    STORE(data+GPU_ERRORS,errors);
    STORE(data+GPU_ATTEMPTS,attempts);
    STORE(data+GPU_OVERLAP,overlap);
    STORE(data+GPU_OLD_SUM,old_sum);
    STORE(data+GPU_FINISHED,1UL);
    STORE(data+STATE,completed==iterations ? 1UL : 2UL);
}

// Serialized handoff isolates return-value/compare-exchange usage from races.
__attribute__((reqd_work_group_size(32,1,1)))
__kernel void atomic_handoff(__global ulong *data, ulong iterations,
                            ulong max_attempts, unsigned unused) {
    if (__builtin_amdgcn_workitem_id_x()) return;
    STORE(data+READY,1UL);
    ulong errors=0, completed=0;
    for (ulong round=0;round<iterations;++round) {
        const ulong base=0x1234567800000000UL+round*64UL;
        for (ulong phase=1;phase<=4;++phase) {
            ulong attempts=0;
            const ulong turn=round*4+phase;
            while (LOAD(data+GO)!=turn) {
                if (++attempts>=max_attempts || LOAD(data+ABORT)) goto finished;
            }
            ulong old,expected;
            bool success;
            if (phase==1) {
                old=__scoped_atomic_fetch_add(data+COUNTER,5UL,__ATOMIC_SEQ_CST,__MEMORY_SCOPE_SYSTEM);
                if (old!=base) ++errors;
            } else if (phase==2) {
                old=__scoped_atomic_exchange_n(data+COUNTER,base+20UL,__ATOMIC_SEQ_CST,__MEMORY_SCOPE_SYSTEM);
                if (old!=base+12UL) ++errors;
            } else if (phase==3) {
                expected=base+30UL;success=CAS(data+COUNTER,&expected,base+40UL);
                old=expected;
                if (!success || expected!=base+30UL) ++errors;
            } else {
                expected=base+41UL;success=CAS(data+COUNTER,&expected,0xdeadbeefUL);
                old=expected;
                if (success || expected!=base+40UL) ++errors;
            }
            STORE(data+HANDOFF_RESULT+phase-1,old);
            STORE(data+GPU_ERRORS,errors);
            STORE(data+GPU_PROGRESS,turn);
        }
        completed=round+1;
    }
finished:
    STORE(data+GPU_ERRORS,errors);
    STORE(data+STATE,completed==iterations ? 1UL : 2UL);
}
