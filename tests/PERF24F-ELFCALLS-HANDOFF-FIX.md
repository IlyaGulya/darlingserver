# perf#24f-fix-elfcalls-handoff (task #104) — DCC __dyld func-lookup handoff fixed

Status: **FIX LANDED (dyld commit 7a93752), elfcalls RIP=0 crash GONE. A separate later crash remains**
(libsystem_c `_flockfile`, a data-fixup defect) — tracked as a follow-up bead. Prod restored
byte-identical (doctor ALL GREEN). dyld-only change; no builder/cache change.

## Root cause (confirmed live)
Darling launches via the dyld2-classic path (`ImageLoaderMachOCompressed`). At the end of
`ImageLoaderMachOCompressed::doBind`, the normal path calls `setupLazyPointerHandler(context)`
(`ImageLoaderMachO.cpp:2100`), which walks each image's `__DATA,__dyld` section and patches its two
pointers to point into dyld proper:
- `dd->dyldLazyBinder = &stub_binding_helper`
- `dd->dyldFuncLookup = &_dyld_func_lookup`

A **DCC-substituted image returns early from `doBind`** (its fixups were applied once via the cache-native
table in `rebase()`), so it **never reached `setupLazyPointerHandler`**. Consequently libdyld's `__dyld`
section kept its baked `dyldFuncLookup` value = **0x0**.

Chain to the crash: libSystem's `libSystem_initializer` builds `_libkernel_functions` with
`.dyld_func_lookup = _dyld_func_lookup` (a **self/this-image bind** → resolves to libdyld's exported
`__dyld_func_lookup`, which is a **trampoline** that jumps through `_myDyldSection[1]` = the `__dyld`
section's `dyldFuncLookup`). With that slot = 0x0, libsystem_kernel's `_mach_driver_init` did:
```
movq (__libkernel_functions), %rax ; callq *0x68(%rax) [= dyld_func_lookup, jumps to NULL slot...]
```
and the lookup for `"__dyld_get_elfcalls"` returned NULL → `_mach_driver_init` called the NULL result →
RIP=0 (the task #103 capture).

Live proof (temporary diagnostic, since reverted):
```
dyld[DCC2-handoff]: /usr/lib/system/libdyld.dylib __dyld sect@0x… size=56 dyldFuncLookup 0x0 -> 0x…856ad0
```
libdyld's `dyldFuncLookup` was 0x0 and the fix set it to dyld-proper's `_dyld_func_lookup`.

## The fix (dyld3/../src/ImageLoaderMachOCompressed.cpp)
In `doBind`'s DCC skip branch, call `this->setupLazyPointerHandler(context)` before returning, so a
DCC-substituted image participates in the exact same `__dyld` func-lookup handoff as a disk-loaded image.
The DCC image's `__DATA` is COW-mapped RW, so the two pointer writes are safe. Added a RED regression
gate: after the handoff, hard-fail (`dyld::halt`) with an explicit message if
`_dyld_func_lookup("__dyld_get_elfcalls")` still resolves NULL — an explicit failure beats a silent RIP=0.

Verified live: the handoff now runs for all 38 substituted images (libSystem.B, libsystem_kernel, libdyld,
…); libdyld's `dyldFuncLookup` is patched; the elfcalls RIP=0 crash is GONE.

## Remaining crash (follow-up bead: perf#24f-fix-flockfile-datafixup)
DCC5-ON boot now advances PAST elfcalls and crashes later:
```
SIGSEGV code=0x80 fault=0x0  RIP in libsystem_c.dylib __TEXT _flockfile+0x12
insn@RIP: 48 8b 7b 68   = movq 0x68(%rbx), %rdi     ; f->_lock, where rbx = the FILE* arg
RBX = RDI = 0x6165726874705f6d = ASCII "m_pthrea"  ; a STRING, not a FILE*
RCX/RAX in RW region (+0x52c); stack[+0x00] also = "m_pthrea"
```
`_flockfile(FILE*)` was called with a garbage FILE* whose bytes read as the string fragment "m_pthrea"
(part of a symbol name like `..._m_pthread...`). A data slot that should hold a pointer holds
symbol-name/string bytes → a DCC **data fixup / rebase** target is wrong for libsystem_c (or an image it
binds), OR a stdio global (`__sF`) pointer wasn't rebased. This is a distinct defect from the elfcalls
handoff (which is fixed) and is the next thing to chase (re-use the temp dserver crashcap dump on the
duct-tape signal path + the full.dcc5 image-table classifier).

## Acceptance status for #104
- [x] Root cause = missing `__dyld` handoff for DCC images; fixed in the dyld2 substitution path.
- [x] Live: libdyld `dyldFuncLookup` 0x0 → dyld `_dyld_func_lookup`; `__dyld_get_elfcalls` now resolves;
      elfcalls RIP=0 crash GONE (crashcap2 no longer sees the elfcalls fault).
- [x] DCC5 still: maps exactly 3 regions, substitutes libSystem.B/libsystem_kernel/…, applies 27082
      fixups once, skips the normal fixup path, 0 staleness rejects (all observed in the live log).
- [x] No builder changes.
- [x] Prod restored byte-identical (doctor ALL GREEN).
- [ ] DCC5-ON /usr/bin/true guest-child exit 0 — NOT yet: blocked by the separate `_flockfile`
      data-fixup crash above. That is the follow-up bead, not the elfcalls handoff.
