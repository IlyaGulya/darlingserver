# perf#24b — libSystem closure shared-mapping design (upper-bound first)

**DESIGN + upper-bound, no production code.** Follows perf#24a (VERDICT B+D: the `/usr/lib/system` closure is
*needed-but-repeated* — ~40 dylibs × 3 VMAs = ~120 closure VMAs/proc, 64% of the 187 VMAs `zap_pte_range` walks
at exit, driving perf#22a's 88% teardown). GOAL: decide whether collapsing those ~120 VMAs into a small
packed shared mapping is worth building, and if so specify the minimal correct form. Same 200-file clang `-j8`
workload. prod dserver `835946f9` + mldr `f0cd2a82` untouched. `DPREFIX=~/.darling`.

## 1. Preflight (clean session) — PASS
Correct `DPREFIX=~/.darling` (not `DARLING_PREFIX`, per perf#24a). One launchd (14411) under one server (14396),
6-daemon baseline set (launchd/opendirectoryd/memberd/securityd/shellspawn/iokitd), no duplicate sessions.
`shellspawn` + `make --version` (GNU Make 3.81) sane. (9 leftover `-j8` zombies parented to launchd — hold only
PID slots, no VMAs/no server slot; harmless.) Binaries verified `835946f9`/`f0cd2a82`.

## 2. Upper-bound result (Track 1) — the GO/NO-GO experiment
Controlled host micro-benchmark modelling one process's closure lifecycle (map → fault → munmap), ×400 procs,
scaled to the **real perf#24a closure** (40 dylibs, 30 MB mapped, 9.4 MB faulted). CURRENT = 40×3 VMAs;
CACHE = 6 packed regions (TEXT-shared / LINKEDIT-shared / DATA-COW + slack), same total & faulted bytes.

| pattern | VMAs/proc | mmap (load) | fault-in | munmap (teardown) | minor faults |
|---------|----------:|------------:|---------:|------------------:|-------------:|
| CURRENT 40×3 | 120 | 36.5 ms | 258 ms | 75.0 ms | 960,803 |
| CACHE 6-region | 6 | 1.9 ms | 212 ms | 8.8 ms | 960,800 |
| **delta** | **20× fewer** | **19×** | ~1.2× | **8.5×** | **1.0× (none)** |

**VMA-count isolation** (faulted bytes held constant at 9.4 MB, only VMA count varied — separates the
VMA-driven teardown cost from the page-zap floor):

| VMAs/proc | munmap µs/proc |
|----------:|---------------:|
| 6 (cache) | 138 |
| 40 | 212 |
| **120 (current closure)** | **419** |
| 240 | 628 |

Teardown = **~140 µs fixed page-zap floor (set by faulted pages, VMA-independent) + a per-VMA component**. The
native 120-VMA figure (419 µs) matches perf#24a's **measured 480 µs/proc `exit_mmap` under Darling** — so the
model is faithful. Collapsing 120 → 6 VMAs cuts teardown **419 → 138 µs ≈ 3×**, saving **~300 µs/proc**.

### Build-wide projection (×400 procs)
| axis | current | cache | build-wide save | notes |
|------|--------:|------:|----------------:|-------|
| exit_mmap teardown | 480 µs/proc | ~162 µs/proc | **~127 ms** (192→65 ms) | 66% of teardown; the perf#22a 88% bucket |
| mmap load (VMA create) | ~97 µs/proc | ~5 µs/proc | **~37 ms** | 19× fewer create calls |
| **minor faults** | — | — | **~0 (UNCHANGED)** | same bytes fault regardless of VMA packing |

**~164 ms serial-equivalent build-wide mapping-side save** — a substantial fraction of perf#22a's kernel mm
bucket (12.8% on-CPU, teardown-dominated). **This is well above "мелочь" → GO on the mapping side.** The
crucial caveat: **the cache does NOT reduce minor faults or fault-in time** — those are set by *faulted bytes*,
not VMA count. So the win is the **VMA create + teardown** (and the ~1.1 ms dylib-loading pre-main phase), not
the fault-in. On `-j8` the ~164 ms serial save spreads across 8 cores (≈ tens of ms wall on the 1.15 s build);
the honest framing is it attacks the **kernel mm / teardown CPU bucket**, which is real cross-core cost, more
than it moves single-build wall.

## 3. Design result — ACCEPT (minimal form), with correctness constraints
A **Darling system-closure cache**: one prebuilt image for `libSystem.B` + the ~33 `/usr/lib/system/libsystem_*`
subs, mapped as a small fixed set of packed regions per process, while **dyld still logically exposes each as a
separate dylib** (Mach-O identity preserved). Segment plan:
- **TEXT** (r-x) and **LINKEDIT** (r--): packed contiguous, **MAP_SHARED read-only** across processes → one VMA
  each, page-cache shared, faulted once system-wide (this is where the minor-fault story *could* improve vs the
  per-process anon model, if pages are genuinely shared — a real cache advantage the upper-bound's anon model
  understates).
- **DATA** (rw): packed but **MAP_PRIVATE / COW** → one VMA, per-process private on write.
- dyld treats cache images as already located in one shared mapping plane (fixed preferred addresses,
  binds precomputed against the packed layout).

**Required dyld changes** (in `src/external/dyld` — dyld3/AllImages + the image loader): a cache reader that (a)
maps the packed regions, (b) registers each contained dylib in the image list with its slice of the mapping,
(c) runs the existing init order over them. This is the macOS `dyld_shared_cache` concept, minimally.

**Correctness constraints (must all hold):**
- **Image list / dladdr / dlsym**: each sub-dylib must still appear as its own image with correct
  `[start,end)` so `_dyld_image_count`, `dladdr`, `dlsym(RTLD_*)`, and symbol lookup resolve exactly as today.
- **Init order**: must preserve the source-level order (see §4) — libkernel → … → libxpc — even though the
  images share one mapping; the cache carries a precomputed init table.
- **ASLR / rebase**: the cache is built at fixed preferred addresses; either map at those addresses (disable
  per-image slide within the cache) or apply one slide to the whole plane and precompute binds against it.
  Bind/rebase is only 7.6% of pre-main so precomputing it is a bonus, not the goal.
- **Writable DATA COW**: DATA must be MAP_PRIVATE so per-process writes (e.g. `errno`, malloc zones, dispatch
  globals) don't leak across processes — this is the one segment that stays per-process.
- **fork/exec**: the cache mapping must survive `fork` (COW like any mapping) and be re-established on `exec`
  (dyld re-maps it); no server round-trip in the hot path.
- **codesign / path identity**: Darling doesn't enforce macOS codesigning here, but the cache must preserve
  each dylib's install-name/path identity so `otool -L`, `@rpath`, and DYLD_PRINT_LIBRARIES still report the
  real paths (tools and the guest see "many dylibs").

**Not rejected** — the upper-bound clears the bar. But note the win is bounded to VMA/teardown/load; if a later
measurement shows the shared-page fault-in advantage is small under Darling's fault path, the cache is still
justified by the ~127 ms teardown + ~37 ms load, just not more.

## 4. libSystem.B initializer audit (Track 2, outcome D — ranked)
`DYLD_PRINT_INITIALIZERS`: **434 initializer calls total — 430 are clang's own** (LLVM static ctors: option
registration, target tables = real compiler work, **not addressable**), **2 from libSystem.B, 2 from libc++**.
`DYLD_PRINT_STATISTICS` (×5, stable): **libSystem.B init ≈ 0.9–1.2 ms (~30–37%)**, clang init ≈ 0.6 ms (~20%).
So the addressable init cost is the **single libSystem.B initializer**, decomposed at source
(`src/external/libsystem/init.c:230–267`) into its sub-inits, in execution order:

`__libkernel_init` → `__libplatform_init` → `__pthread_init` → `_libc_initializer` → `__malloc_init` →
`__keymgr_initializer` → `_dyld_initializer` → `__pthread_late_init` → `libdispatch_init` → `_libxpc_initializer`

These are what fault the closure resident (explaining perf#24a's 100%-resident `libsystem_*`). Most are
mandatory POSIX/Mach runtime bring-up (kernel/platform/pthread/libc/malloc/dispatch). **Plausible laziness
candidates**: `_libxpc_initializer` (many CLI tools never touch XPC) and `__keymgr_initializer` — but proving
they're safe to defer is its own bead. The shared mapping removes VMA/teardown/load but **does NOT remove this
~1 ms init** — the two levers are orthogonal, which is exactly why the audit was run alongside.

## 5. Recommendation — **C) both, ordered: shared-cache prototype FIRST, then init laziness**
- **First: perf#24c — shared-closure-cache prototype.** It attacks the larger and better-understood cost (the
  ~120→~6 VMA collapse → ~127 ms teardown + ~37 ms load build-wide, the perf#22a 88%-teardown bucket) and its
  correctness surface is bounded and well-specified (§3). Prototype scope: build the packed cache for
  `libSystem.B` + `libsystem_*`, teach dyld to map+register it, gate behind an env flag, validate image
  list/dlsym/dladdr/init-order/fork-exec, then A/B the exit_mmap/VMA/load deltas on the 200-file build.
- **Then: perf#24d — libSystem.B init laziness**, but only after the cache lands and only if the ~1 ms init is
  still a meaningful share of the (by then smaller) per-process cost. Start with the two low-risk candidates
  (`libxpc`, `keymgr`) behind proof they're unused by cc1/driver.
- Not B-first (init is the smaller, riskier lever). Not D/STOP (the mapping upper-bound cleared the bar).

### Explicitly NOT done / NOT touched
No cache built, no dylibs pruned (they're live), no init made lazy, no process reuse, no vfork/spawn, no
ring/psynch/fd-band/NOFILE, no LLVM patch, bind/rebase not optimized (7.6%, deliberately last). prod dserver
`835946f9` + mldr `f0cd2a82` untouched; sysctls unchanged.

## Method / artifacts
Track 1: `p24b-upperbound.c` (full lifecycle, sizes from perf#24a) + `p24b-vmacost.c` (VMA-count isolation,
faulted bytes constant) → `p24b-upperbound-results.txt`. Native host C (isolates the kernel VMA/pagetable/zap
cost, which is file-agnostic; matched to Darling's measured 480 µs/proc). Track 2: `DYLD_PRINT_INITIALIZERS` +
`DYLD_PRINT_STATISTICS` (Darling dyld honors them) + source read of `libsystem/init.c`. No PMU; guest
Mach-O userspace syms don't resolve in perf. Reuses perf#24a + perf#22a + perf#23a.
