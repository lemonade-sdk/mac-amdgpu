#pragma once
// All offsets are 64-bit words. CPU/GPU ownership sequences and cancellation
// have separate 128-byte regions; requests/results are owned by one side at a time.
#define MAC_MAILBOX_REQUEST_SEQUENCE 0
#define MAC_MAILBOX_COMPLETION_SEQUENCE 16
#define MAC_MAILBOX_READY 32
#define MAC_MAILBOX_ABORT 48
#define MAC_MAILBOX_STATE 64
#define MAC_MAILBOX_COUNT 80
#define MAC_MAILBOX_SLOT 96
#define MAC_MAILBOX_REQUESTS 128
#define MAC_MAILBOX_RESULTS 512
#define MAC_MAILBOX_CAPACITY 64
#define MAC_MAILBOX_WORDS 2048
// Request = {operation, operand, compare}; returned value is always the old value.
#define MAC_MAILBOX_REQUEST_STRIDE 3
