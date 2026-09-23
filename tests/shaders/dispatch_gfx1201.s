// Kernarg at s[0:1]: data pointer (u64), addend (u32), reserved (u32).
// s2 = workgroup X, v0 = local X. Launch 32 threads per group.
// Four groups cover 128 inputs; output starts at byte 1024.
.text
.p2align 8
.globl dispatch_vector
dispatch_vector:
    s_load_b128 s[4:7], s[0:1], 0
    s_wait_kmcnt 0
    s_lshl_b32 s3, s2, 5
    v_add_u32 v1, s3, v0
    v_lshlrev_b32 v1, 2, v1
    global_load_b32 v2, v1, s[4:5]
    s_wait_loadcnt 0
    v_add_u32 v2, s6, v2
    global_store_b32 v1, v2, s[4:5] offset:1024
    s_wait_storecnt 0
    s_endpgm
