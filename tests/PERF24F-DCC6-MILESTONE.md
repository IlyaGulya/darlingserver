# perf#24f — DCC6 MILESTONE (decision + perf record)

Status: **MILESTONE — DCC6 crossed the hard correctness boundary.** The packed 3-region closure cache
(DCC) now runs a real `clang` build end-to-end under a flag. This is a **prototype, flag-gated, NOT
default-on.** This record freezes what works, how, the measured result, and what is deliberately left.

---

## 1. Why a no-code-change cache was impossible on x86_64
The goal is to collapse the ~120 per-process VMAs of the libSystem closure (40 dylibs × 3 segments) into
a few shared regions. On Apple's ARM64 shared cache this is "free" because fixups are pre-applied and the
loader skips them. On Darling/x86_64 that path does not exist, and three things block a naive pack:
- **Single-slide model** (perf#24c-gate): dyld resolves every segment via one TEXT slide
  (`MachOLoaded::getSlide`). Packing segments into per-permission regions moves DATA/LINKEDIT relative to
  TEXT, so their runtime addresses no longer equal `loadedAddr + (seg.vmaddr − TEXT.vmaddr)` unless the
  segment vmaddrs are rewritten region-relative.
- **RIP-relative TEXT→DATA refs** (perf#24c2d / #97): x86_64 `__text` is full of `disp32(%rip)` loads and
  `ff25` stubs whose baked displacement assumes the original TEXT↔DATA spacing. Region-packing changes
  that spacing → every such ref points at the wrong place → `"__text"` SIGSEGV. There is no way around
  rewriting the code bytes (perf#24e feasibility: 42002 sites, 0 int32-overflow, boundable via
  LC_FUNCTION_STARTS).
- **Opcode fixup engine assumes contiguity**: dyld's rebase/bind opcode walk assumes each image is one
  contiguous TEXT|DATA|LINKEDIT span; a region-scattered image breaks it (perf#24c2a).

Conclusion: a correct cache on x86_64 **requires build-time code rewriting + a precomputed fixup table +
a dyld reader that bypasses the normal fixup path.** That is DCC.

## 2. Why DCC6 works — the five pieces
1. **3-region layout** (perf#24c1): all `__TEXT` → RX region, all `__DATA` → RW, all `__LINKEDIT` → RO;
   one arena, one slide. Segment vmaddrs rewritten region-relative so the single-slide model holds. This
   is what actually collapses the VMAs (same-permission contiguity → 3 mappings, not 120).
2. **TEXT RIP-relative rewrite** (perf#24f / #100): the builder rewrites every same-DATA `disp32(%rip)`
   site, `__stubs` `ff25`, and `__stub_helper` lea+jmp in the packed `__TEXT` so TEXT→DATA refs land
   correctly after packing. Data-in-text constant pools (24 B in libsystem_m) are detected and never
   rewritten. Totals: 42002 sites, 23182 same-DATA rewritten, 18820 same-TEXT left, 0 overflow.
3. **DCC fixup table** (perf#24c2b): rebase/bind resolved at build time into a cache-native table keyed
   by region+offset; the reader applies it once over the COW RW region and never lets a DCC image enter
   the normal opcode fixup path. 27082 fixups (23407 rebase + 3664 bind), 0 unresolved.
4. **Original-vmaddr export translation** (perf#24f #106→#107): the export trie stores addresses
   original-image-relative. `exportedSymbolAddress` must translate them through the ORIGINAL segment table
   to the rewritten arena — else a `__DATA` export (`___stdinp`) resolves into the RX region and bash reads
   a load-command string (`"m_pthrea"`) as a FILE* → `_flockfile` SIGSEGV. DCC6 records
   `orig_vmaddr`/`orig_vmsize` per segment; `DCC2Reader::translateVmaddr` maps original→arena. Magic bump
   rejects any pre-#107 cache.
5. **elfcalls handoff** (perf#24f #104): a DCC-substituted image skipped `doBind`'s
   `setupLazyPointerHandler` tail, leaving libdyld's `__dyld.dyldFuncLookup` = 0x0 →
   `dyld_func_lookup("__dyld_get_elfcalls")` = NULL → RIP=0. Fix: run the handoff in the DCC doBind-skip
   branch + RED halt gate.

## 3. Live acceptance (all GREEN, DCC6 cache full.dcc6, flag-gated)
| Test | Result |
|---|---|
| `env /usr/bin/true` | rc 0 |
| `env /usr/bin/false` | rc 1 |
| `sh -c 'echo …'` / bash | prints, rc 0 (was the `_flockfile` crash) |
| shellspawn (pipe / for-loop / subshell / `&&`) | all correct, rc 0 |
| `clang -c` 1 file | valid Mach-O 64-bit x86_64 `.o` |
| `clang -O2 -c` 200 files `-j8` | 200 objs, no crash |
| RED: old DCC5 cache under DCC6 reader | `bad magic (need DCC6)` → clean hard-fail |
| DCC invariants | enabled, 40 images, single arena, substitutes libSystem/libsystem_*, 0 staleness |

## 4. VMA result (the mechanism, proven live)
- Live cc1 host-side `/proc/<pid>/maps` line count: **OFF ≈190 → ON ≈62–72** (~65% fewer).
- The 40-dylib closure's **120 VMAs (40×3) collapse to 3 DCC regions** (RX/RW/RO), one arena, one slide.

## 5. Perf (functional, warm)
- 200-file `-j8`, warm guest: **OFF 1.27s / ON 1.06s** wall.
- Caveat: 200 tiny files is spawn/noise-heavy and short — it proves correctness and shows the VMA
  collapse, but **under-represents the teardown/load win**. A larger real build is needed to quantify it
  (perf#24g).

## 6. Known remaining buckets (NOT addressed by DCC6)
- **libSystem init still runs** every process (perf#24b: libSystem.B ~1 ms, initializers). DCC collapses
  mappings, not init work. Init laziness is a separate, later effort.
- **Minor page faults ≈ unchanged**: the same bytes fault in regardless of how they're packed (perf#24a).
  DCC's win is VMA create/teardown + load, not fault-in.
- **Real clang CPU** (cc1 compile work) is untouched and dominates a real build.

## 7. Default status
- **NOT default-on. Prototype, flag-gated** (`DARLING_DYLD_DCC2=1` + `DARLING_DYLD_DCC2_PATH`).
- Flag absent → dyld behaves exactly as before; invalid/stale cache → clean hard-fail, never half-state.
- Enabling by default would require: perf#24g quantification, a cache-build+staleness story in the normal
  deploy, and coverage beyond the 40-dylib clang closure.

## Provenance (LOCAL commits, never pushed)
dyld `aec6a4a` (fix/dyld-dcc2-reader) · darlingserver `0e3e6c5`+`10bc4fa` (perf/shmem-ring-abi-validator) ·
manifest handoff `d91ad00`. Cache: `dcc5-builder <prefix>/libexec/darling closure-list.txt full.dcc6`
(md5 49e652d1, guest `/private/var/root/full.dcc6`). Build dyld against xnu@a0328833, then restore
perf/shmem-ring-guest. Prod restored byte-identical (dyld 79b22273 both / dserver 835946f9 / mldr
f0cd2a82); `west darling-doctor` ALL GREEN. Detail: PERF24F-107-EXPORTED-SYMBOL-TRANSLATION.md,
PERF24F-ELFCALLS-HANDOFF-FIX.md, PERF24F-CRASHCAP2-ROOTCAUSE.md, PERF24F-FLOCKFILE-INVESTIGATION.md.

## Next (perf#24g) — measurement, not surgery
Larger real-codebase build A/B (OFF vs ON, same rebuilt dyld / same closure / same install_root). Collect
wall/user/sys, cc1 count, per-cc1 VMA, exit_mmap/zap teardown, dyld load time, substituted-image count,
staleness rejects, correctness (object count + build success). Do NOT start init laziness, default-enable,
optimize the packer, or rewrite more dyld paths before that.
