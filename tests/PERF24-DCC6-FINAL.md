# perf#24 — FINAL decision record (DCC6 shared-cache prototype)

Status: **CLOSED — successful but bounded.**

> **perf#24 result.** The DCC6 shared-cache prototype is functionally proven and kernel-measured.
> It materially reduces VM teardown / VMA work. It improves launch/teardown-heavy tiny-process
> workloads. It does **not** materially move CPU-bound large clang builds.
>
> In one line: **DCC6 is a launch-churn optimization, not a compiler-CPU optimization.**

This record supersedes nothing — it rolls up the whole perf#24 arc (#79–#109) into one decision.
Detail lives in the per-task records: `PERF24F-DCC6-MILESTONE.md`,
`PERF24F-107-EXPORTED-SYMBOL-TRANSLATION.md`, `PERF24F-ELFCALLS-HANDOFF-FIX.md`,
`PERF24G-DCC6-LARGER-BUILD-AB.md` (incl. the direct teardown A/B).

---

## 1. Why a no-code-change cache was impossible on x86_64
Collapsing the ~120 per-process VMAs of the libSystem closure (40 dylibs × 3 segments) into a few
shared regions is "free" on Apple's ARM64 shared cache (fixups pre-applied, loader skips them).
On Darling/x86_64 that path does not exist, and three things block a naive pack:
- **Single-slide model** (perf#24c-gate): dyld resolves every segment via one TEXT slide, so
  packing DATA/LINKEDIT into separate regions breaks their runtime addresses unless segment
  vmaddrs are rewritten region-relative.
- **RIP-relative TEXT→DATA refs** (perf#24c2d / perf#24e): x86_64 `__text` is full of
  `disp32(%rip)` loads and `ff25` stubs whose baked displacement assumes the original TEXT↔DATA
  spacing; region-packing changes that spacing → wrong target → `"__text"` SIGSEGV. Only
  code-byte rewriting fixes it (perf#24c2d-fix2 proved delta-preserving layout ⟂ VMA-collapse).
- **Opcode fixup engine assumes contiguity**: dyld's rebase/bind walk assumes each image is one
  contiguous TEXT|DATA|LINKEDIT span; a region-scattered image breaks it (perf#24c2a).

Conclusion: a correct cache on x86_64 **requires build-time code rewriting + a precomputed fixup
table + a dyld reader that bypasses the normal fixup path.** That is DCC.

## 2. Why DCC6 works (the four load-bearing pieces + the layout)
0. **3-region layout** (perf#24c1): all `__TEXT`→RX, `__DATA`→RW, `__LINKEDIT`→RO; one arena, one
   slide; segment vmaddrs rewritten region-relative. This is what collapses 120 VMAs → 3.
1. **TEXT RIP-relative rewrite** (perf#24f): builder rewrites every same-DATA `disp32(%rip)`,
   `__stubs` `ff25`, `__stub_helper` lea+jmp so TEXT→DATA refs land after packing. 42002 sites,
   23182 rewritten, 0 int32 overflow; data-in-text pools (libsystem_m) detected & left alone.
2. **DCC fixup table** (perf#24c2b): rebase/bind resolved at build into a cache-native table keyed
   by region+offset; reader applies it once over the COW RW region and never lets a DCC image
   enter the normal opcode fixup path. 27082 fixups, 0 unresolved.
3. **Original-vmaddr export translation** (perf#24f #107): export-trie offsets are
   original-image-relative; `exportedSymbolAddress` must translate them through the ORIGINAL
   segment table to the rewritten arena, or a `__DATA` export (`___stdinp`) resolves into the RX
   region and bash reads a load-command string (`"m_pthrea"`) as a FILE* → `_flockfile` SIGSEGV.
   DCC6 records `orig_vmaddr`/`orig_vmsize` per segment; magic bump rejects any pre-#107 cache.
4. **elfcalls handoff** (perf#24f #104): a DCC-substituted image skipped `doBind`'s
   `setupLazyPointerHandler` tail, leaving `__dyld.dyldFuncLookup`=0 → `__dyld_get_elfcalls`=NULL
   → RIP=0. Fix runs the handoff in the DCC doBind-skip branch + a RED halt gate.

## 3. Functional acceptance (all GREEN, flag-gated)
| Test | Result |
|---|---|
| `env /usr/bin/true` | rc 0 |
| `env /usr/bin/false` | rc 1 |
| `sh`/bash `-c 'echo …'`, shellspawn (pipe/for/subshell/&&) | correct, rc 0 (was the `_flockfile` crash) |
| `clang -c` 1 file | valid Mach-O 64-bit x86_64 `.o` |
| 500-file `clang -O2 -c … -j8` | 500 objs, byte-identical codegen OFF vs ON |
| RED: pre-#107 cache under DCC6 reader | `bad magic (need DCC6)` → clean hard-fail |

## 4. Kernel proof (bpftrace, host `comm==mldr`, one 500-file build/arm — perf#24g-teardown)
| kernel teardown metric | OFF | ON (DCC6) | delta |
|---|---|---|---|
| **unmap_vmas / free_pgtables** (VMAs torn down) | 150 239 | 32 584 | **−78.3 %** (74 → 16 / process) |
| **zap_pte_range** calls | 551 217 | 185 600 | **−66.3 %** |
| **exit_mmap** total CPU | 1.080 s | 0.888 s | **−17.7 %** (−191 ms / build; mean 532 → 437 µs) |
| `/proc/<cc1>/maps` lines | 193 | 65 | −66 % (independent confirmation) |

The VMA collapse is real at the mm layer (unmap_vmas −78 %). But `exit_mmap` CPU drops only −17.7 %,
**sublinear in VMA count** — resident pages still get zapped and each process unmaps its own view of
the shared DCC regions (same physics as perf#24a "minor faults ≈ unchanged"). The teardown bucket is
genuinely reduced and now quantified (191 ms/build), but it is not proportional to the VMA count.

## 5. Workload conclusion
- **Tiny, launch/teardown-heavy workloads benefit.** Milestone 200-tiny-file `-j8`: OFF 1.27 s /
  ON 1.06 s (−17 % wall) — there per-process setup+teardown is a large fraction of the work.
- **CPU-bound builds do not move.** perf#24g 500-file `-O2`: OFF == ON at 13 s guest / 14 s host.
  The 191 ms teardown saving is ~1.4 % of a compile-CPU-dominated build → invisible at wall.
- The mechanism (VMA collapse) is **robust across build size**; the *wall* payoff is **conditional
  on the workload's launch/teardown fraction.**

## 6. Status
- **Prototype, flag-gated** (`DARLING_DYLD_DCC2=1` + `DARLING_DYLD_DCC2_PATH`). Flag absent → dyld
  behaves exactly as before; invalid/stale cache → clean hard-fail, never a half-state.
- **NOT default-on.** Productization is a separate hardening effort: cache-build + staleness story
  in the normal deploy, coverage beyond the 40-dylib clang closure, ABI-version discipline on the
  cache format, and a fallback-on-mismatch policy.
- All perf#24 code is **LOCAL commits, never pushed.** Prod restored byte-identical after every
  measurement (dyld 79b22273 both / dserver 835946f9 / mldr f0cd2a82); `west darling-doctor` GREEN.

---

## If performance work continues — two honest branches (NOT started here)

**A. Launch-heavy path** (goal: faster `true`, `shellspawn`, configure-style loops, Ruby/brew
startup, thousands of tiny processes). DCC6 removed the VMA churn; the remaining startup bucket is
**init**: libSystem.B init ≈ 0.9–1.2 ms/proc (perf#24b), which DCC does not touch. Next would be
**perf#24d / perf#25: libSystem.B init laziness** — but start with *audit/proof only*:
- which libSystem init subcalls are actually needed before `main`?
- are libxpc / keymgr / libdispatch lazy-init candidates?
- do `env` / `true` / `sh` / `clang` actually use them, or pay for them dead?

**B. CPU-bound build path** (goal: the 14 s large clang build). Runtime launch work is no longer the
lever — DCC has already taken it. The honest next step is **compiler/userspace CPU attribution**
(better symbols / PMU / a native-ish baseline to compare cc1 against), or simply **stop**: DCC will
not give more wall on this class of workload.

**Do NOT** start either branch as a "DCC fix" — DCC6 is complete for what it is. The next lever is
init (branch A) or compiler CPU (branch B), chosen by which workload matters.
