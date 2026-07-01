# perf#24a — dyld / mapping reduction reconnaissance

**RECON ONLY, no code changes.** Follows perf#23a (VERDICT C: process reuse infeasible → dyld/mapping
reduction is the next lever). Attributes the *per-dylib* cost (loaded vs mapped vs faulted vs init vs bind) for
the clang driver and cc1 on the same 200-file clang `-j8` workload, to decide which of outcomes A–E holds.
Prod dserver `835946f9` + compact-fd mldr `f0cd2a82` (both live in the shared install root
`darling-prefix/`), untouched.

## Preflight (mandatory — perf#23a env-stability gotcha)
**Root cause of a perf#23a labelling error found here:** the launcher prefix env var is **`DPREFIX`** (default
`~/.darling`), **not `DARLING_PREFIX`** — so every prior `DARLING_PREFIX=…homebrew-test` command actually ran
against **`~/.darling`**. Both prefixes share the *same* binaries (server `835946f9`, mldr `f0cd2a82` — verified
via `/proc/<pid>/exe`; the prefix dir holds only the guest FS), so the perf#23a *findings* are valid — only the
prefix label was wrong. perf#24a runs against `~/.darling` with `DPREFIX` set explicitly. Preflight verified:
**exactly one launchd under one server**, no duplicate sessions, shellspawn + `make --version` sane.

## Instruments (Darling dyld honors the DYLD_PRINT_* envs)
`DYLD_PRINT_LIBRARIES` (loaded list + order), `DYLD_PRINT_SEGMENTS` (per-dylib VMAs + sizes),
`DYLD_PRINT_INITIALIZERS` (which dylibs run init), `DYLD_PRINT_STATISTICS` (pre-main phase timing).
Plus `/proc/<pid>/smaps` (faulted RSS per dylib) and kprobe `exit_mmap` (teardown vs VMA count). No PMU.

## Pre-main phase timing (DYLD_PRINT_STATISTICS) — driver ≈ cc1
Two near-identical blocks per compile (driver + cc1), **~3.7 ms pre-main each**:

| phase | time | share |
|-------|-----:|------:|
| **initializers** | **~2.1 ms** | **58%** — of which **libSystem.B init alone ~1.1 ms (30%)** |
| dylib loading | ~1.1 ms | 30% |
| rebase/binding | ~0.28 ms | **7.6%** |
| ObjC setup | ~0.11 ms | 3% |

→ **bind/rebase is small (outcome C weak); the pre-main cost is initializers + loading.** Only **libSystem.B
and libc++.1 run their own initializers**; the ~33 `libsystem_*` sub-libs run none. clang's own binary runs ~40
init functions (its LLVM static ctors — real compiler work, not addressable here).

## The closure — WHY 40 dylibs load (the mechanism)
`otool -L /usr/lib/libSystem.B.dylib` shows **libSystem is an umbrella that hard-links (LC_LOAD_DYLIB, eager,
not lazy) ~33 `/usr/lib/system/libsystem_*` sub-libraries**. clang's *direct* deps are only 4 (`libncurses`,
`libz`, `libSystem`, `libc++`); **libSystem transitively pulls the entire `/usr/lib/system/` set** into every
process that links it — i.e. every macOS binary. That is why all 40 load, all map, and (see below) all fault.

## Per-dylib ranking (one process; driver and cc1 load the IDENTICAL 41-dylib set — each loaded exactly 2×)

Each dylib maps **3 VMAs** (`__TEXT` r-x / `__DATA` rw- / `__LINKEDIT` r--). Faulted = resident (smaps Rss) of
a live cc1. `class`: **compiler** = plausibly needed by the compiler; **closure** = pulled only by the libSystem
umbrella. `init` = runs its own initializer.

| dylib | VMAs | mappedKB | faultedKB | touch% | init | class |
|-------|-----:|---------:|----------:|-------:|:----:|-------|
| libncurses.5.4 | 3 | 21628 | 3616 | 16 | - | compiler |
| libsystem_c | 3 | 1216 | 676 | 55 | - | closure |
| libsystem_kernel | 3 | 752 | 500 | 66 | - | closure |
| libdispatch | 3 | 872 | 496 | 56 | - | closure |
| libc++.1 | 3 | 732 | 480 | 65 | YES | compiler |
| libobjc.A | 3 | 484 | 432 | 89 | - | closure |
| libdyld | 3 | 560 | 392 | 70 | - | closure |
| libcopyfile | 3 | 1004 | 372 | 37 | - | closure |
| libsystem_sandbox | 3 | 480 | 320 | 66 | - | closure |
| libsystem_malloc | 3 | 260 | 244 | 93 | - | closure |
| libxpc | 3 | 280 | 216 | 77 | - | closure |
| libc++abi | 3 | 236 | 204 | 86 | - | compiler |
| libcorecrypto | 3 | 156 | 148 | 94 | - | closure |
| libsystem_info | 3 | 396 | 136 | 34 | - | closure |
| … (26 more, all `class=closure` except libz/libSystem.B/libunwind/libcompiler_rt) | 3 | ≤270 | ≤132 | 34–100 | libSystem.B=YES | mixed |

Full table in `p24a-ranked.txt`. Summary: **40 dylibs/process → ~120 dylib VMAs; 33 of 40 are pure closure
(libSystem-pulled, not compiler-needed); only 7 are compiler-relevant; only 2 run own initializers.**

## No dead weight — every dylib is faulted (rules out outcome A)
**Zero dylibs load-but-never-touch.** Every one has faulted pages; the small `libsystem_*` are **100% resident**
(fully touched), the large ones partially (code-page faults). Total per process: **~30 MB mapped dylib bytes,
~9.6 MB faulted (31% resident** — the gap is sparse text in big libs like libncurses, not unused libraries).
The reason nothing is dead: the libSystem umbrella + its init eagerly reference the whole `/usr/lib/system/`
graph. So the closure is **not prunable as "unused" (A)** — it's structurally instantiated per process.

## VMA / teardown correlation (ties to perf#22a's 88% teardown)
Live clang: **189 total VMAs, of which 120 (64%) are the dylib closure** (40×3); ~29 anon rw. Kprobe over a
100-file `-j8` slice: **437 procs torn down, 210 ms total `exit_mmap`, 480 µs/proc, avg 187 VMAs/proc** (all in
the 128–256 bucket). So the closure is **~64% of every VMA that `zap_pte_range` walks on exit** — the direct
mechanism behind perf#22a's finding that `exit_mmap` teardown is 88% of the mm bucket. Fewer dylibs → fewer
VMAs → less per-process teardown, multiplied across ~400 procs/build (~16.4 k identical load+map+fault+teardown
cycles: 400 × 41).

## VERDICT — mixed **B + D**, and NOT A.

- **NOT A (prune unused dylibs):** every dylib is faulted; nothing is dead weight. The closure can't be cut as
  "unused" — it's the libSystem umbrella's eager, hard-linked graph. (This corrects the perf#22a/23a suspicion
  that "~52 dylibs none of which the compiler needs" were removable — they're *not compiler-needed* but they
  *are* touched, because libSystem instantiates them.)
- **B (too many VMAs / mapping churn) — the dominant, addressable cost.** 120 of 187 per-process VMAs are the
  closure; this drives the 88% `exit_mmap` teardown (perf#22a) *and* the 30% dyld-loading pre-main phase, ×400
  procs. Major faults are ~0 (warm cache) — so the lever is **fewer mmap regions / shared mapping**, not disk.
- **D (initializers) — a real but smaller second lever.** Initializers are 58% of the (small, ~3.7 ms) pre-main
  window, concentrated in **libSystem.B's init (~1.1 ms)** which is what faults the closure resident. clang's
  own ~40 static ctors are real LLVM work (not addressable). So the init lever = **libSystem.B's startup**, not
  the sub-libs.
- **NOT C:** bind/rebase is only ~7.6% of pre-main (~0.28 ms) — a bind cache is not the win.
- **NOT E:** the dominant per-process cost is the closure's map+fault+teardown (a Darling/libSystem structural
  cost), not clang/libc++ compute — addressable without touching LLVM.

### Recommended next bead — perf#24b (design): closure mapping reduction
Because the closure is *needed-but-repeated*, the lever is **map it once and share it across processes, not
prune it**. Concretely, in priority order:
1. **Shared-cache-like single mapping of the `/usr/lib/system/` set** — one prebuilt image mapped (ideally
   COW-shared) per process instead of 33 separate dylibs × 3 VMAs, collapsing ~99 closure VMAs toward a handful.
   This is the classic macOS dyld shared cache; the win here is **fewer VMAs → less `exit_mmap`/`zap` teardown
   and fewer minor faults**, not disk speed (major faults already ~0).
2. **libSystem.B initializer audit (outcome D):** attribute *what* its ~1.1 ms init does under Darling and
   whether the closure-faulting portion is reducible/deferrable — the concrete second lever if the shared-cache
   route is heavy.
VMA coalescing without a full cache (e.g. mapping sub-libs contiguously) is a lighter intermediate if the cache
proves too invasive. **Do NOT prune dylibs (A) — they're live.**

### Explicitly NOT done / NOT touched
No shared cache built, no dylibs removed, no process reuse, no vfork/spawn, no ring/psynch/fd-band/NOFILE, no
LLVM patch. Measurement only. prod dserver `835946f9` + mldr `f0cd2a82` untouched; kernel sysctls unchanged.

## Method / reproduce / gotchas
`DPREFIX=~/.darling` (NOT `DARLING_PREFIX` — the launcher ignores the latter). `DYLD_PRINT_{LIBRARIES,SEGMENTS,
INITIALIZERS,STATISTICS}` all honored by Darling dyld — the primary instruments (no per-dylib /proc race needed
for VMAs/sizes; smaps only for faulted RSS, sampled off a deliberately-large compile so cc1 lives long enough).
`otool -L` for the umbrella structure. kprobe `exit_mmap` reads `mm_struct->map_count` for the VMA correlation.
Guest procs are comm=mldr; guest Mach-O userspace symbols don't resolve in perf (kernel kprobes do); no PMU;
bpftrace str keys <48 B. Artifacts: `p24a-clang-full.txt` (segments+stats), `p24a-perdylib-1proc.txt` (VMA+KB),
`p24a-faulted.txt` / `p24a-ranked.txt` (faulted+class table), `p24a-smaps.txt`, `p24a-exittime.txt`.
Reuses perf#23a + perf#22a + perf#21b + perf#20a.
