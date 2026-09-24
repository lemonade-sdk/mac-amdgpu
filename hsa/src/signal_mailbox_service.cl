#include "signal_mailbox_layout.h"
#define LOAD(p) __scoped_atomic_load_n((p),__ATOMIC_ACQUIRE,__MEMORY_SCOPE_SYSTEM)
#define RELAXED_LOAD(p) __scoped_atomic_load_n((p),__ATOMIC_RELAXED,__MEMORY_SCOPE_SYSTEM)
#define STORE(p,v) __scoped_atomic_store_n((p),(v),__ATOMIC_RELEASE,__MEMORY_SCOPE_SYSTEM)
#define RELAXED_STORE(p,v) __scoped_atomic_store_n((p),(v),__ATOMIC_RELAXED,__MEMORY_SCOPE_SYSTEM)
// One wave, one active lane: the CPU never RMWs the canonical signal value.
// Every target update is a native GPU atomic; request/completion use ownership.
__attribute__((reqd_work_group_size(32,1,1)))
__kernel void signal_mailbox_service(__global ulong *mailbox,__global ulong *arena,
                                    ulong slots,ulong max_attempts) {
    if (__builtin_amdgcn_workitem_id_x()) return;
    STORE(mailbox+MAC_MAILBOX_READY,1UL);
    for (ulong sequence=1;sequence!=0;++sequence) {
        ulong attempts=0;
        while (LOAD(mailbox+MAC_MAILBOX_REQUEST_SEQUENCE)!=sequence) {
            if (LOAD(mailbox+MAC_MAILBOX_ABORT)) goto stopped;
            if (++attempts>=max_attempts) goto cancelled;
        }
        if (LOAD(mailbox+MAC_MAILBOX_ABORT)) goto stopped;
        const ulong slot=RELAXED_LOAD(mailbox+MAC_MAILBOX_SLOT);
        if (slot>=slots) goto invalid;
        __global ulong *signal=arena+slot*8UL+1UL;
        const ulong count=RELAXED_LOAD(mailbox+MAC_MAILBOX_COUNT);
        if (!count || count>MAC_MAILBOX_CAPACITY) goto invalid;
        for (ulong i=0;i<count;++i) {
            __global ulong *request=mailbox+MAC_MAILBOX_REQUESTS+i*MAC_MAILBOX_REQUEST_STRIDE;
            const ulong operation=RELAXED_LOAD(request),value=RELAXED_LOAD(request+1),compare=RELAXED_LOAD(request+2);
            ulong old=0;
            switch (operation) {
            case 1: case 7: old=__scoped_atomic_exchange_n(signal,value,__ATOMIC_SEQ_CST,__MEMORY_SCOPE_SYSTEM);break;
            case 2: old=__scoped_atomic_fetch_add(signal,value,__ATOMIC_SEQ_CST,__MEMORY_SCOPE_SYSTEM);break;
            case 3: old=__scoped_atomic_fetch_sub(signal,value,__ATOMIC_SEQ_CST,__MEMORY_SCOPE_SYSTEM);break;
            case 4: old=__scoped_atomic_fetch_and(signal,value,__ATOMIC_SEQ_CST,__MEMORY_SCOPE_SYSTEM);break;
            case 5: old=__scoped_atomic_fetch_or(signal,value,__ATOMIC_SEQ_CST,__MEMORY_SCOPE_SYSTEM);break;
            case 6: old=__scoped_atomic_fetch_xor(signal,value,__ATOMIC_SEQ_CST,__MEMORY_SCOPE_SYSTEM);break;
            case 8: old=compare;__scoped_atomic_compare_exchange_n(signal,&old,value,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST,__MEMORY_SCOPE_SYSTEM);break;
            case 9: old=LOAD(signal);break;
            default: goto invalid;
            }
            RELAXED_STORE(mailbox+MAC_MAILBOX_RESULTS+i,old);
        }
        STORE(mailbox+MAC_MAILBOX_COMPLETION_SEQUENCE,sequence);
    }
stopped:
    STORE(mailbox+MAC_MAILBOX_STATE,1UL);return;
cancelled:
    STORE(mailbox+MAC_MAILBOX_STATE,2UL);return;
invalid:
    STORE(mailbox+MAC_MAILBOX_STATE,3UL);
}
