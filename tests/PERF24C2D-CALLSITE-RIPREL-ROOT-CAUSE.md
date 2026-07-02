# perf#24c2d-callsite — ROOT CAUSE of the "__text" SIGSEGV in full-closure DCC2

Status: **ROOT CAUSE FOUND** (2026-07-02). No production code changed. Offline analysis only.
Prod baselines untouched/recoverable: dyld 79b22273 (both copies) / libsystem_kernel 6bd251c3 /
mldr f0cd2a82 / dserver 835946f9. DCC2 dyld reader = fix/dyld-dcc2-reader @ 02696ac.
DCC2 builder = darlingserver @ e5d9fe8 (perf/shmem-ring-abi-validator).

## The crash (from task #96 crashcap)

Guest SIGSEGV, `err=0x14` (user + INSTRUCTION-FETCH) + `trapno=0xE` (#PF), with
`RIP == faultaddr == cr2 == 0x747865745F5F` = the ASCII bytes **"__text"**. So the guest
executed a `call`/`jmp` **through a function pointer whose value is a `section_64.sectname`
string**, not a code address. Crash in libSystem.B's first initializer, `rdi = libc++ __TEXT+0x1028`.

## What it is NOT (previously ruled out, re-confirmed here)

Offline (`wildchk2`, `fxdump`) + live-post-fixup scans proved **no DCC2 pointer-slot fixup
(rebase / bind_internal / bind_extern / bind_extern_lazy) produces a header-zone pointer**.
`fxdump full.dcc2 libc++.1` → rebase=1776 bind_int=333, HEADER-ZONE-FLAGGED=0. The `__got` and
`__la_symbol_ptr` cache values are internally consistent. So the bug is **not** a wrong/missing
data-pointer fixup.

## What it IS — DCC2 region-packing invalidates baked RIP-relative displacements

x86-64 `__stubs` entries are `ff 25 <disp32>` = `jmp *disp32(%rip)`, targeting the image's
`__la_symbol_ptr`. The `disp32` is a **fixed immediate in the instruction bytes**, computed at
link time for the ORIGINAL per-image TEXT->DATA spacing.

DCC2 rewrites `LC_SEGMENT_64.vmaddr` and `section_64.addr` to region-relative absolutes, so:

| section            | original (on-disk libc++ x86_64) | DCC2 (full.dcc2)        |
|--------------------|----------------------------------|-------------------------|
| `__stubs`          | 0x5de80                          | RX **0x65e80**          |
| `__la_symbol_ptr`  | 0x680e8  (DATA follows TEXT)      | RW **0x52c0e8** (far)   |

The stub's `disp32 = 0xa262` is **unchanged** (a fixup table only rewrites pointer *slots*, never
instruction immediates). So `jmp *0xa262(%rip)` from 0x65e86 now computes
`0x65e8c + 0xa262 = 0x700e8` = **libc++abi's __TEXT header** (image 2, packed right after libc++
in the RX region), instead of libc++'s real `__la_symbol_ptr` at 0x52c0e8.

`dcc2-dumpq full.dcc2 libc++ 0x700e8 4` → `[0x700e8] = 0x000000010001cfe0` (garbage header bytes).
`dcc2-stubscan.py` → ALL libc++ stubs land in 0x700e8+ (libc++abi header). When a stub target lands
exactly on `<image>+0x68` (first `section_64.sectname` = "__text"), the read qword is
`0x747865745f5f` and the `jmp` fetches from there → the captured crash.

**This is a model flaw, not a per-image bug.** It breaks every RIP-relative reference from `__TEXT`
code into `__DATA` (`__got`, `__la_symbol_ptr`, `__nl_symbol_ptr`, RIP-relative `__const`/`__data`/
cstring/objc refs). The perf#24c-gate "single uniform slide" model is INSUFFICIENT for x86-64: it
keeps addresses self-consistent only if the whole image slides rigidly, but region-packing slides
`__TEXT` and `__DATA` by DIFFERENT amounts.

## Why Apple's dyld shared cache doesn't hit this

Apple's CacheBuilder preserves each image's ORIGINAL intra-image segment spacing (TEXT->DATA delta
invariant) inside the cache. Images are packed, but the per-image TEXT<->DATA distance is unchanged,
so baked RIP-relative displacements stay valid.

## Fix options (for the next stage — NOT implemented in this task)

- **(A) RECOMMENDED — preserve per-image TEXT<->DATA spacing.** Lay out the regions so that for each
  image, `(DATA_addr_in_cache - TEXT_addr_in_cache) == (orig_DATA_vmaddr - orig_TEXT_vmaddr)`. Keeps
  all code immediates valid AND still collapses VMAs (same-perm contiguity across images). This is
  exactly what a real shared cache does. Constrains packing density but is the only correct option.
- (B) Rewrite every RIP-relative disp32 in code that targets a moved section. Needs disassembly/reloc
  info dyld doesn't have at build time; brittle. Rejected.
- (C) Keep each image's segments at original relative offsets and slide whole images only. Abandons
  cross-image region contiguity = abandons the VMA-collapse win. Rejected.

## Reproduce

```
cd tools/closure-cache
cc -O0 -o dcc2-extractrx dcc2-extractrx.c && ./dcc2-extractrx full.dcc2 rx.bin
cc -O0 -o dcc2-inspect   dcc2-inspect.c   # (== job-tmp inspect) shows __stubs / __la_symbol_ptr addrs
cc -O0 -o dcc2-dumpq     dcc2-dumpq.c
cc -O0 -o dcc2-fxdump    dcc2-fxdump.c
objdump -D -b binary -m i386:x86-64 --start-address=0x65e80 --stop-address=0x65f60 rx.bin
python3 dcc2-stubscan.py         # all libc++ stubs -> libc++abi header zone
```

Tools added: `dcc2-extractrx.c` (dump RX region), `dcc2-dumpq.c` (dump+classify qwords by vmaddr),
`dcc2-fxdump.c` (per-image fixups, flag header-zone targets), `dcc2-stubscan.py` (stub disp scan).
