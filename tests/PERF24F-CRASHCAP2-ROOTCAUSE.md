# perf#24f-crashcap2 (task #103) — DCC5 early-init fault-PC CAPTURED + ROOT-CAUSED

Status: **DONE — fault-PC captured, crash root-caused.** The DCC5-ON early-init SIGSEGV that blocked
perf#24f-live-A/B (#102) is now fully diagnosed. It is a **dyld↔libsystem_kernel init-handoff defect for a
DCC-substituted image**, NOT a rip-rel rewrite bug (the rewrite is provably complete: 0 uncovered exec
bytes) and NOT a VMA-layout bug (the arena maps as exactly 3 regions, single slide, as designed). Prod
restored byte-identical throughout (doctor ALL GREEN).

## How it was captured (the method that finally worked)
A TEMPORARY diagnostic dump was added in darlingserver's duct-tape signal path
(`dtape_thread_process_signal`, duct-tape/src/thread.c) on the first `EXC_BAD_ACCESS` (SIGSEGV/SIGBUS).
This runs INSIDE the guest's PID namespace and AFTER Darling's Mach-exception translation, which is exactly
why host gdb / guest sigexc / strace all failed before (the 6 attempts in PERF24F-LIVE-AB-BLOCKED.md). The
dump used `thread_get_state(x86_THREAD_STATE64)` for registers, `dtape_hooks->task_read_memory` (the
server's namespace-correct `process_vm_readv`) for the stack, and `task_get_memory_region_info` /
`task_get_next_region` to classify addresses against the guest VMAs. The instrumentation was reverted after
capture (logging-only, no behavior change, dserver-only, restored to baseline 835946f9).

## The capture
```
linux_signal=11 (SIGSEGV) code=0x1 fault_addr=0x0
RIP=0x0  RSP=0x7fffffdfe728  RBP=0x7fffffdfe750     => a CALL through a NULL function pointer
retaddr[RSP] = <arena_rx_base>+0x43b124
region(retaddr): start=<arena_rx> pages=1324 (0x52c000) off=0x114000 prot=5 (R-X)   <- the DCC RX region
```

## Classification vs full.dcc5 (arena RX base observed 0x740333ba0000)
The arena is **exactly 3 regions** — RX file_off=0x114000 size=0x52c000 (prot 5), RW vm_base=0x52c000
(prot 3), RO vm_base=0x628000 (prot 1) — one contiguous block, single slide. **VMA collapse works.**
The multiple prot=5 VMAs elsewhere in the walk are launchd's own non-cached libs, not arena fragmentation.

Caller frames (RX offset → libsystem_kernel.dylib __TEXT):
- `0x43b124` → **`_mach_driver_init`+0x24** (the faulting call site)
- `0x42cd99` → `_mach_init_doit`+0x9
- `0x4339f0` → `__libkernel_init`+0x30   (dyld's first initializer call into libkernel)
- RDI `0x46baf2` → the cstring `"__dyld_get_elfcalls"` (packed in __TEXT `_hex+0x472`)

## The call site (`_mach_driver_init`, source vmaddr 0x2f100)
```
2f10c: leaq  __libkernel_functions(%rip), %rax   ; &__libkernel_functions (__DATA,__common bss @0x6af38)
2f113: movq  (%rax), %rax                        ; rax = __libkernel_functions[0]  (dyld-populated fn table)
2f116: leaq  0x309d5(%rip), %rdi                 ; rdi = "__dyld_get_elfcalls"
2f11d: leaq  -0x18(%rbp), %rsi
2f121: callq *0x68(%rax)                          ; dyld dlsym("__dyld_get_elfcalls") -> stored at -0x18(%rbp)
2f124: callq *-0x18(%rbp)                         ; <-- CRASH: the returned pointer is NULL
```

## Root cause (non-rewrite)
`_mach_driver_init` reads `__libkernel_functions` (a `__DATA,__common` table dyld fills at the libkernel
init handoff), calls its dlsym at `*0x68(%rax)` to resolve `"__dyld_get_elfcalls"`, and calls the result.
Under DCC5 the dlsym returns **NULL** for a DCC-substituted libsystem_kernel — dyld's function-pointer
handoff / internal symbol resolver does not serve `__dyld_get_elfcalls` for a cached image. This is
Darling's **elfcalls bootstrap** (`elfcalls_wrapper.c`: `_elfcalls` / `__elfcalls` / `_elfcalls_get_pointer`),
the mldr↔libsystem_kernel bridge. The DCC reader substitutes the image and applies fixups, but does not
reproduce the `__libkernel_functions` / `__dyld_get_elfcalls` handoff the normal (non-cached) load path
performs.

Important detail: `*0x68(%rax)` (the dyld dlsym) DID run — we reached 0x2f124 — so
`__libkernel_functions[0]` itself was a valid pointer. The failure is specifically that
`dlsym("__dyld_get_elfcalls") == 0` for the substituted image. So the handoff table is present but the
per-image symbol lookup for the DCC image is not wired.

## Confirmed NOT the cause
- rip-rel rewrite: exec-coverage gate = 0 uncovered exec bytes; RIP=0 (a NULL call), not a bad disp.
- the 11 BIND_EXTERN_LAZY sentinels: all are libcommonCrypto→libcorecrypto crypto primitives
  (`_ccchacha20*`, `_ccec_*blind`, `_cccmac_cbc`, `_cch2c`, `_cchkdf_*`), none on the early-init path.
- VMA layout: arena is a single 3-region block, single slide.

## Next (fix bead perf#24f-fix-elfcalls-handoff)
Make dyld's DCC substitution path perform the SAME libkernel function-table / `__dyld_get_elfcalls`
handoff for a substituted image as the disk path: register the DCC-substituted image so dyld's internal
`__dyld_func_lookup` / libkernel init handoff resolves `__dyld_get_elfcalls`, OR route the
DCC-substituted libsystem_kernel through the same init/handoff registration as a normally-loaded image.
This lives in the DCC substitution code in dyld3/Loading.cpp (`isDCCImage` / substituted-image
registration + notify/init handoff), NOT the builder or the cache.
