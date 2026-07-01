# perf#22a — dyld / startup-mapping attribution

**RECON, no code changes.** Splits the post-D21b "dyld / pagefault / startup" bucket into actionable causes on
the same 200-file clang `-j8` workload (compact-fd mldr `f0cd2a82` live, `dup_fd` stays dead, prod dserver
`835946f9` untouched). Answers the critical fork: is the cost **fork PTE-copy before immediate exec**
(→ spawn/vfork lever) or something else? Method: static read of the spawn path + bpftrace kprobe timing on the
mm kernel functions + mmap/fault census, same discipline as perf#21a/b (no PMU on this host).

## Spawn shape (static)
- `sys_fork` (fork.c) = real `fork()` / `clone(SIGCHLD)` — a **full COW address-space clone** (page-table copy).
- **`sys_posix_spawn` also goes through `sys_fork()`** then execs in the child — it is **NOT** vfork /
  `CLONE_VFORK`. So even posix_spawn (which shells/make/libc use) pays the full fork COW clone before exec.
  This is exactly the "fork a big address space only to exec immediately" pattern — so we measured how much it
  actually costs.

## Kernel mm cost split (guest `mldr`, kprobe timing, 3 builds)
| kernel op | total time | share | calls | per-call |
|-----------|-----------:|------:|------:|---------:|
| **`exit_mmap`** (address-space teardown) | **1.488 s** | **88%** | 2,508 | 593 µs |
| `copy_page_range` (fork COW PTE-copy) | 0.203 s | 12% | 237,935 | 853 ns |

**`exit_mmap` is 7.3× the fork PTE-copy.** Faults (3 builds): 2.69M total, **minor 2.63M, major 155 (~0)** —
libraries are warm in the page cache, so this is not disk I/O; it's minor-fault-in + map/unmap churn. mmap
syscalls ~256K/3builds ≈ **206 mmaps per process**; `vm_mmap` file-maps = 19,733 over 93 execs (40-file build)
≈ **212 file mappings per exec**.

## The critical fork — RESOLVED: fork PTE-copy is NOT the lever
The hypothesis was that forking a huge address space only to exec immediately dominates. **It does not.** The
COW clone (`copy_page_range`) copies only *page-table entries* (853 ns/call, 12% of mm cost) — cheap, because
it's copy-on-write, not a content copy. The dominant cost is the **teardown** (`exit_mmap → zap_pte_range`,
88%): every short-lived process maps ~206 regions (the *same* dyld + libsystem_* + libc++ set, over and over),
faults them in, then unmaps them all on exit. Across ~415 processes/build that's ~85K maps + ~85K unmaps of
identical libraries.

So a spawn/vfork fast path (skipping the COW clone) would save only ~12% of the mm bucket (~0.2 s/3 builds) —
**not worth it**, and it wouldn't touch the 88% teardown or the userspace dyld work.

## Per-process repetition (bucket 3)
Every cc1/driver/sh/path_helper: ~206–212 file mappings, then full teardown, ~0 major faults. The mapped set
is the **same system dylibs repeated in all ~415 processes** — the workload re-does identical dyld load +
library mapping + teardown 415 times per build. This is the root that feeds *both* the kernel mm bucket (12.8%
on-CPU: fault-in + teardown) and the userspace **dyld 21%** (bind/rebase/lookup on top of the same repeated
mapping).

## Ranked startup buckets by total contribution (post-D21b)
1. **Per-process teardown + repeated mapping churn** — kernel `exit_mmap` 88% of the mm bucket (~12.8% on-CPU)
   + the userspace **dyld ~21%** that re-runs the same load every process. Same root: **process churn** — the
   build spawns ~415 short-lived processes that each map, fault-in, and tear down the identical library set.
2. Fork COW PTE-copy — 12% of the mm bucket (~1.5% on-CPU). Cheap; not a lever.
3. Library open/stat (VFS metadata) — small (openat ~68K, warm; not fault-bound).
4. Major faults / disk — negligible (155 total; warm cache).

## VERDICT: **E — process reuse / teardown avoidance is the next lever.**
Not A (spawn/vfork): the fork PTE-copy is only 12% of the mm cost — the "fork huge AS then exec" is real but
COW-cheap, so a vfork/`CLONE_VFORK` fast path would recover ~1.5% on-CPU at most and is not worth the
correctness risk. Not B (dyld bind cache) as the *primary*: dyld's 21% is large, but it's a *symptom* of
re-running the full loader per process — the lever that addresses both the 21% userspace and the 12.8% kernel
teardown is **not re-doing it 415 times**. Not C/D (mapping/VFS): faults are warm and cheap. The dominant,
addressable cost is **process churn**: hundreds of short-lived processes each mapping + tearing down the same
libraries.

### Recommended next bead (perf#23, recon → design): compiler/loader process reuse
Investigate a **persistent-worker / process-reuse** direction so the identical dyld load + library mapping +
teardown isn't repeated 415×/build — e.g. whether a warm cc1/loader pool or a shared library-mapping scheme is
feasible under Darling's process model, and what the correctness constraints are (each cc1 is a fresh macOS
process today). This is the lever that attacks both the 88% teardown and the 21% userspace dyld at once.
Secondary, only if process reuse proves infeasible: a **dyld-side** reduction (shared-cache-like mapping /
prebinding) to cut the per-process load cost — but that's bucket-B follow-on, measured *after* the reuse
question.

### Explicitly NOT the lever / NOT touched
Fork PTE-copy / spawn-vfork fast path (measured, only 12%). NOFILE cap (D21c), ring, psynch, fd-band, clang/LLVM
— untouched. No optimization, no caching implemented. `dup_fd` confirmed still dead.

## Method / reproduce
Static: `fork.c`/`posix_spawn.c` in the emulation. Dynamic: `p22a-mm.bt` (kprobe copy_page_range / exit_mmap /
__handle_mm_fault + page-fault software events + mmap/munmap tracepoints, comm=mldr) around 3 builds;
`p22a-dylib3.bt` (execs / opens / vm_mmap_pgoff) for per-exec mapping count. Guest Mach-O userspace symbols
don't resolve in perf (DSO+kernel only) — the mm split comes from kernel kprobes, which resolve. Warm
shellspawn, `timeout -s KILL`, prod restored/untouched.
