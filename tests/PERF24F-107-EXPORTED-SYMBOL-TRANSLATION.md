# perf#24f (#107) — DCC exported-symbol original-vmaddr translation (DCC6)

Status: **FIXED + LIVE GREEN.** The `_flockfile` crash (#106) is resolved. DCC6-ON now runs
`env true`/`false`, `sh`/`bash`, shellspawn, and `clang -c` (1-file and 200-file `-j8`) with the VMA
collapse intact (cc1 ~190 → ~65 VMAs). dyld+builder commits LOCAL; prod restored byte-identical
(doctor ALL GREEN).

## Root cause (from #106, proven)
`ImageLoaderMachOCompressed::exportedSymbolAddress` computed a DCC image's export as
`fMachOData + trieOffset`. The export-trie offset is **ORIGINAL-image-relative** (linker wrote it
relative to the original mach_header, TEXT vmaddr 0), but a DCC image is **region-packed** and its
`fMachOData` points at the RX/__TEXT region. So a `__DATA` export — e.g. `___stdinp` at original
image offset `0xc8890` — resolved to `RX_base + 0xc8890`, landing inside the RX region on
load-command bytes (`"m_pthrea"` = offset 24 of `/usr/lib/system/libsystem_pthread.dylib`). bash's
`check_dev_tty` does `fileno(*___stdinp)` → passed that garbage FILE* to `_flockfile` →
`movq 0x68(%rbx),%rdi` SIGSEGV. `/usr/bin/true` was unaffected because it binds no `__DATA` export
from a DCC image. Frame-walk kill-shot (#106):
```
RIP libsystem_c _flockfile+0x12   RBX=RDI=0x6165726874705f6d "m_pthrea"
frame[1]=bash 0x10001f78d: movq 0x8d896(%rip),%rax; movq (%rax),%rdi; call _fileno   (rax=&___stdinp)
PROBE bash_got[0x1000ad020] -> &___stdinp = RX+0x39c890  (= RX + libsystem_c_TEXT 0x2d4000 + 0xc8890)
PROBE *(___stdinp) = 0x6165726874705f6d "m_pthrea"
```
`resolved(0x39c890) − libsystem_c_TEXT_region_off(0x2d4000) = 0xc8890` = the `___stdinp` original
vmaddr, confirming the naive `fMachOData(RX)+trieOffset` computation.

## Why the reader couldn't fix it without a format change
The trie offset is original-image-relative, but DCC5 `dcc_seg.vmaddr` is the **rewritten**
region-relative value (`dcc5-builder.c: ds->vmaddr = vmb + roff`). Repacking changes inter-segment
spacing (e.g. libsystem_c __DATA: original vmaddr `0xc5000`, rewritten `0x5ac000`), and the original
vmaddr is not reconstructable from the cache (page-alignment ambiguity). The reader needs the
original seg table.

## The fix (DCC6)
- **Cache format** (`dcc5-format.h`): `dcc_seg` gains `orig_vmaddr`/`orig_vmsize` (the pre-repack
  LC_SEGMENT_64 values). New magic `DCC6` (0x44434336) / version 6 so any pre-#107 cache is rejected.
- **Builder** (`dcc5-builder.c`): fills `orig_vmaddr/orig_vmsize` from the source segment; writes DCC6.
  Layout/region-packing/RIP-rel rewrite/fixup table UNCHANGED (same 40 dylibs, 27082 fixups, identical
  rewrite totals; only the seg struct grew + magic bumped).
- **Reader** (`DCC2Reader`): `translateVmaddr(imageIndex, A)` finds the seg with
  `orig_vmaddr ≤ A < orig_vmaddr+orig_vmsize` and returns
  `arena + regions[region_idx].vm_base + region_off + (A − orig_vmaddr)`. `exportedSymbolAddress` uses
  it for DCC images (REGULAR / THREAD_LOCAL / resolver-stub) with a RED gate: hard-fail if a vmaddr
  maps to no segment (refuse a garbage address). Reader now requires DCC6 (rejects DCC5).

## Offline gates (GREEN)
- DCC6 header: magic=DCC6, 40 images, orig_vmaddr correct — libsystem_c `__TEXT orig 0x0/0xc5000`,
  `__DATA orig 0xc5000/0xa000`, `__LINKEDIT orig 0xcf000/0x61000` (matches on-disk dylib exactly).
- Translation of `___stdinp` (off 0xc8890): → arena-rel `0x5af890`, inside RW region
  `[0x52c000,0x628000)` (NOT the naive `0x39c890` in RX). DATA export resolves into RW/__DATA. ✓

## Live acceptance (DCC6 cache full.dcc6, dyld 9bcba4a→final, all via `env … DCC2_PATH=…/full.dcc6`)
- `env /usr/bin/true` → rc 0 ; `env /usr/bin/false` → rc 1. ✓
- `sh -c 'echo SH_OK_DCC6'` → **SH_OK_DCC6 printed, rc 0** (was the _flockfile crash). ✓
- shellspawn: `echo A; echo B | cat; for i in 1 2 3; do echo n$i; done; /usr/bin/true && echo AND_OK`
  → all output correct, rc 0. ✓
- `clang -c t.c -o t.o` → valid Mach-O 64-bit x86_64 object, rc 0. ✓
- `clang -O2 -c` 200 files `-j8` → 200 objs, no crash. Wall (warm): **OFF 1.27s / ON 1.06s**. ✓
- **VMA collapse (headline):** live cc1 host-side `/proc/<pid>/maps` line count —
  **OFF ≈190 VMAs → ON(DCC6) ≈62–72 VMAs** (~65% fewer; the 40-dylib/120-VMA closure → 3 regions). ✓
- DCC invariants: reader enabled, 40 images, single arena, substitutes libSystem.B/libsystem_c/
  libsystem_kernel/libdispatch/…, 0 staleness rejects, 0 halts. ✓

## RED arms
- Old DCC5 cache under a DCC6 reader → `dyld[DCC2]: bad magic (need DCC6)` → `cache invalid/stale`
  hard-fail (never a wrong resolution). ✓
- Naive `fMachOData+trieOffset` for a DATA export → produces `"m_pthrea"` (the #106 capture IS this
  arm). ✓
- Export vmaddr covered by no segment → `exportedSymbolAddress` RED gate `dyld::halt`. ✓

## Commits (LOCAL only)
- dyld `aec6a4a` (branch fix/dyld-dcc2-reader): reader translateVmaddr + exportedSymbolAddress + DCC6.
- darlingserver `0e3e6c5` (branch perf/shmem-ring-abi-validator): DCC6 format + builder.
- Cache built with `dcc5-builder <prefix>/libexec/darling closure-list.txt full.dcc6`
  (install_root = `<prefix>/libexec/darling`; guest path `/private/var/root/full.dcc6`; md5 49e652d1).

## Boundary / honest status
- [x] Root cause fixed at the exact class (DCC __DATA/repacked export address translation).
- [x] Live: true/false/sh/bash/shellspawn/clang-1/clang-200-j8 all green; VMA collapse ~190→~65.
- [x] Prod restored byte-identical (dyld 79b22273 both, dserver 835946f9, mldr f0cd2a82); doctor GREEN.
- [x] No RIP-rewrite / 3-region layout / elfcalls-handoff change; only seg-struct grew + magic bump.
- [ ] Perf timing at this file size is spawn-dominated (OFF 1.27 / ON 1.06 warm); a larger real
      codebase build would quantify the exit_mmap/teardown win more precisely (perf#22a: teardown is
      88% of the mm bucket; VMA collapse is the mechanism). The functional A/B + VMA collapse are proven.
