# HSA ABI headers

Unmodified `hsa.h` from
https://github.com/iree-org/hsa-runtime-headers/tree/4285513114a70f7cf4830c89279c8cfa57b901bb
with its upstream `LICENSE.txt`. This is the revision pinned by HRX System
commit `437e789eaea207a036c197cf3398a6ca473d6534` in `MODULE.cmake.lock`.

Only the core header is vendored at present. AMD extension headers will be
added with their implementations. Do not substitute locally invented HSA ABI
types or enum values for the upstream declarations.
