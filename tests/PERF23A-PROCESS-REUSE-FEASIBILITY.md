# perf#23a — process-reuse feasibility and upper-bound

**RECON + upper-bound, no code changes.** Follows perf#22a (VERDICT E: the dominant addressable cost is
process churn — ~415 short-lived procs/build each re-doing identical dyld load + ~52-dylib mapping + fault-in +
`exit_mmap` teardown). This bead asks: **can that churn be attacked by reusing clang/cc1 processes, or is reuse
infeasible so the next lever is dyld/mapping reduction?** Same 200-file clang `-j8` workload, compact-fd mldr
`f0cd2a82` live (`dup_fd` dead), prod dserver `835946f9` untouched. Method: bpftrace exec/exit_mmap/copy_pte
census + `/proc` maps + wall timing + the batching upper-bound experiment (all measured, no PMU on host).

## Block 1+2 — process histogram + clang driver/cc1 split (baseline, 200-file `-j8`)

| binary | execs | note |
|--------|------:|------|
| **clang** (`/Library/Developer/CommandLineTools/usr/bin/clang`) | **401** | **95.9% of all execs** |
| make / sh / bash / rm / mkdir / date / awk / ls / wc / tr / path_helper | 17 | one-time build scaffolding |
| **total** | **418** | |

- The 401 clang execs split **~200 driver + ~200 cc1** — verified by argv: 200 execs have `argv[2]=-isysroot`
  (the `clang -isysroot … -O2 -c -o f.o f.c` driver invocation from the Makefile) and 200 have the cc1 argv
  shape (`clang -cc1 -triple x86_64-apple-macosx… -emit-obj …`). So the build spawns **exactly 2 clang
  processes per source file**: the driver, which forks/execs itself as `clang -cc1`.
- This clang is **Apple LLVM 9.0.0 (clang-900.0.39.2)** — an old generation, no server/reuse mode (see Block 5).
- Kernel mm cost this build: `exit_mmap` **837 calls / 428 ms** vs `copy_page_range` (fork COW PTE-copy)
  **77,543 calls / 60 ms** → **teardown ≈ 7.1× the fork copy** (consistent with perf#22a's 7.3×).

## Mapped-dylib-set repetition — the reuse target

Sampled `/proc/<pid>/maps` of live clang procs during the build:
- **~204 total mappings, ~117 `.dylib` mappings per clang proc; 33 fds; max_fd = 8191** (compact-fd band live,
  no fd at 1048575 — perf#21b holding).
- The **unique dylib set is identical across clang processes**: a **core of ~52 system dylibs** (`libc++.1`,
  `libc++abi`, `libobjc.A`, `libSystem.B`, `libicucore.A`, `libsqlite3.0`, `libMobileGestalt`, `libresolv`,
  `libpam`, `libbsm`, `libbz2`, `libcoretls*`, …) that is a **strict subset** of the larger variant (the
  larger adds 5 graphics libs: `libGL`/`libEGL`/`libX11`/`libXext`/`libXRandR`). None of these are needed by a
  compiler — they are the **dependency closure of the Darling `libSystem`** that dyld pulls in for every guest
  process. Every one of the ~400 clang procs maps, faults-in, and tears down this same set.

## Block 3 — UPPER-BOUND experiment (fewer driver invocations)

Expressed the **same 200 sources** three ways (signal test, NOT a proposed build-system change). Wall is
host-`date` around `darling shell`, warm server, 3 runs each (median). exec/exit_mmap from the census bpftrace.

| variant | wall (median) | total execs | clang execs | `exit_mmap` calls | `exit_mmap` time |
|---------|--------------:|------------:|------------:|------------------:|-----------------:|
| **baseline** — 200 driver calls, `make -j8` | **1.15 s** | 418 | 401 (200 drv + 200 cc1) | 837 | 428 ms |
| batched — **1** driver call, all 200 sources (serial) | 2.58 s | 212 | 201 (1 drv + 200 cc1) | 425 | 172 ms |
| **chunked** — 8 driver calls × 25 sources, run **`-j8`** | **0.65 s** | 217 | 208 (8 drv + 200 cc1) | 443 | 250 ms |

Two findings, and they pull in opposite directions on the two axes:

1. **Process/teardown axis:** collapsing the 200 driver invocations removes ~192–200 driver processes →
   total execs **418 → 212/217 (−49%)**, `exit_mmap` calls **837 → 425/443 (−47%)**, `exit_mmap` time
   **428 → 172/250 ms**. **But the 200 cc1 processes are irreducible** — even a single `clang -c a.c b.c c.c`
   **forks one cc1 per source** (verified with `-###`: `grep -c cc1` = 3 for 3 sources). So batching caps out
   at eliminating the *driver* half of the churn (~50%); the cc1 half — where the actual compile + the same
   ~52-dylib map/teardown happens — stays.

2. **Wall axis — the trap:** the naïve batch (one driver call) is **2.2× SLOWER** (2.58 s vs 1.15 s) because a
   single driver **serialises** its 200 cc1 compilations, throwing away the `-j8` parallelism that hides the
   per-process cost. The win only materialises when you **keep parallelism**: 8 parallel driver calls of 25
   sources each (chunked) is **0.65 s = 44% faster than baseline** while also cutting driver processes.

So "fewer driver invocations" is a **real but bounded** lever: ~44% wall and ~half the teardown, *if and only
if* parallelism is preserved — and it does **nothing** for the 200 cc1 procs, which remain the residual churn.

## Block 4 — zygote sanity

A preloaded-template/zygote (fork a warm process that already has the dylib set mapped, instead of exec+dyld
per cc1) would cut the **dyld load + fault-in** on the child (COW-shared pages) — but perf#22a already showed
the fork COW copy is only 12% of the mm cost and **`exit_mmap` teardown is 88%**, and **every forked child
still runs its own `exit_mmap` on exit**. So a zygote attacks the smaller half and leaves the dominant 88%
teardown fully intact. Zygote is at best a partial dyld-side win, never a teardown win — matches perf#22a's
"D: zygote helps dyld not exit_mmap."

## Block 5 — persistent-worker feasibility

- **clang-900 (LLVM 9) has no server/daemon/persistent mode** (`clang --help` has only `-fmodules-*` cache
  flags — a *content* cache, not a process-reuse server; no `-cc1as`-style daemon, no compile-server
  protocol). cc1 is architecturally **one-TU-per-process**; the driver→cc1 fork-per-source is not a knob.
- A **transparent** clang/cc1 persistent worker would therefore require either (a) an external wrapper protocol
  clang does not speak, or (b) patching LLVM — **neither is a Darling-side lever**, and both are explicitly
  out of scope for this program (the DO-NOT list forbids generic transparent process reuse).
- Even setting the compiler aside, a reuse pool would have to survive the per-process state that makes each cc1
  a fresh macOS process today: cwd/env/umask, signal handlers, temp files, `libSystem`/dyld/ObjC runtime
  init state, malloc arenas & TLS, and diagnostic/output isolation. Under Darling each of these is also a
  **duct-tape'd Mach/BSD emulation** surface, multiplying the correctness risk.

## VERDICT: **C — process reuse is infeasible; the next lever is dyld/mapping reduction.**

The churn is real and dominant (perf#22a), but the two reuse routes are both closed:
- **Persistent clang/cc1 worker (outcome A):** infeasible — this LLVM 9 has no reuse mode, cc1 is
  one-TU-per-process, and a transparent pool is out of scope + high-risk. **Rejected.**
- **Batching / fewer invocations (outcome B):** a **bounded** win (~44% wall, ~half the driver processes) and
  only if parallelism is preserved — but it is a **build-system change, not a Darling runtime lever**, and it
  cannot touch the 200 cc1 processes that carry the residual ~52-dylib map+teardown. Worth noting as a caller
  hint, not a runtime fix. **Not the runtime lever.**
- **Zygote (outcome D):** attacks only the 12% fork/dyld side, leaves the 88% teardown intact. **Not pursued.**
- **Real LLVM (outcome E):** not the story — clang's own CPU is a compiler cost, but the *Darling-specific*
  overhead here is the per-process dyld+mapping+teardown of the ~52-dylib `libSystem` closure, which is
  addressable independent of LLVM. **Not STOP.**

**→ Next lever = dyld / mapping reduction (the perf#22a bucket-B follow-on).** Since we cannot stop re-running
the loader ~400×/build, the win is making each run cheaper: shrink the per-process mapped set and its
teardown. Concretely, the next recon (perf#24) should attribute *which* of the ~52 dylibs are (a) actually
touched by cc1 vs merely pulled in by the `libSystem` dependency closure, (b) reducible via a shared-cache-like
single mapping / prebinding so the fault-in and `exit_mmap` teardown are amortised or shared across procs, and
(c) whether the teardown itself (`exit_mmap → zap_pte_range`, 88%) can be cut by fewer/larger VMAs. That is the
lever that reaches the dominant 88% teardown *and* the 21% userspace dyld without a compiler rewrite.

### Explicitly NOT done / NOT touched
No generic transparent process reuse, no vfork/posix_spawn fast path (perf#22a measured it cheap, 12%), no
zygote, no dyld/LLVM optimization, no ring/psynch/fd-band/NOFILE changes, no build-system change landed (the
chunked variant is a *measurement*). prod dserver `835946f9` and mldr `f0cd2a82` untouched.

## Method / reproduce / gotchas
`p23a-census.bt` = execve tracepoint keyed by short argv slices (comm=mldr) + kprobe timing on
`exit_mmap`/`copy_page_range` (str slices kept < 48 B to avoid BPF stack overflow — the per-dylib census by
string key overflowed, so the set repetition comes from `/proc/maps` sampling + the identical-subset diff).
Harness written into the guest home via a `darling shell` heredoc (guest `/Users/<u>` is a distinct overlay
from the host-side prefix dir — files written host-side aren't visible to the guest). `p23a-batch.sh` (1
driver call) and `p23a-chunk.sh` (8×25, `-j8`) are the upper-bound variants. Wall = host `date` around
`darling shell`; `time -v` on the launcher shows ~0 because the work is in the server/guest procs.
**Env-stability gotcha:** repeated `darling shutdown`/reboot cycles under heavy build churn spawned duplicate
launchd sessions that raced and wedged the guest (make/echo blocked in `__skb_wait_for_more_packets` on an RPC
reply); recovery = kill the wedged server process tree (prod binary file untouched) so the next `darling`
command boots one clean session. Guest Mach-O userspace symbols don't resolve in perf (kernel kprobes do).
Reuses perf#22a + perf#21b + perf#20a briefs.
