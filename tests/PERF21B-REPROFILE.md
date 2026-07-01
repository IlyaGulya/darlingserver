# post-D21b reprofile — re-rank clang/cc1 build bottlenecks after dup_fd elimination

**RECON, no code changes.** Re-runs the perf#21a census on the same 200-file clang `-j8` workload with the
compact-fd mldr (perf#21b) live, to re-rank bottlenecks now that `dup_fd` (the perf#21a #1 lever, 13.3%) is
gone. Same method: `sudo perf record -a -g` + `perf report --comms=mldr --percentage=relative`, `perf stat`,
bpftrace syscall census, `time`. Prod darlingserver `835946f9` untouched; deployed mldr = band-aware
`f0cd2a82`; no fd at 1048575 (re-verified across all guest procs).

## Before/after (perf#21a → perf#21b compact-fd)

| metric | perf#21a | perf#21b | Δ |
|--------|---------:|---------:|--:|
| wall (s) | 1.73 | **1.24** | −28% |
| user (s) | 3.08 | **2.81** | −9% |
| **sys (s)** | **5.72** | **3.90** | **−32%** |
| `[k] dup_fd` (guest on-CPU) | 13.30% (#1) | **0.09%** | ~148× |
| guest syscall time (3 builds) | 62.77 | 48.43 | −23% |

The fix landed exactly where predicted: kernel/sys CPU fell a third, wall ~28%, dup_fd is gone. Page-faults
per build ~887K (perf stat), 12 CPUs, ~1.24 s wall.

## DSO self-time (comm=mldr, relative) — barely moved
| DSO | 21a | 21b |
|-----|----:|----:|
| clang | 37.2% | **35.1%** |
| dyld | 21.4% | **21.0%** |
| libsystem_kernel | 14.6% | 16.3% |
| libsystem_platform | 9.2% | 9.2% |
| libsystem_malloc | 8.6% | 9.2% |

dup_fd was a *kernel* symbol (not attributed to a userspace DSO), so removing it shrank total kernel time and
left the userspace DSO split essentially unchanged. The userspace story is the same: **clang ~35% (real LLVM,
still not dominant); dyld ~21% the standout; libsystem ~35% combined.**

## New kernel on-CPU ranking (top-60 kernel symbols, comm=mldr)
| bucket | share | note |
|--------|------:|------|
| **mmap / pagefault / pagetable** | **12.8%** | **NEW #1** (replaces dup_fd) |
| socket / RPC recv + msg-copy | 7.8% | `recvmsg`/`unix_dgram_recvmsg`/iovec copy — dserver RPC receive |
| syscall entry/exit overhead | 2.9% | `do_syscall_64` |
| fd-table (`dup_fd`…) | 0.8% | **was ~13%, FIXED** |
| (userspace clang/dyld/libsystem, unresolved) | 7.6% of top-60 | bulk of on-CPU is userspace, per-fn unresolved |

**What drives the new #1 (mmap/pagefault) — callgraph attribution:** `zap_pte_range` (top mm symbol) is reached
via `do_exit → exit_mm → __mmput → exit_mmap → unmap_vmas` — i.e. **per-process address-space teardown on
exit**. Combined with page-fault-in (`do_user_addr_fault`, `next_uptodate_folio`, `filemap_map_pages` =
faulting in mmap'd libraries) and page-table copy on fork (`copy_present_pte`), this is the **fork+exec+exit
address-space churn** of a spawn-heavy compile — every clang driver, cc1, sh, and path_helper maps its
libraries in and tears its VM down.

## Syscall time (blocking-inclusive, 3 builds) — for completeness, NOT a CPU bucket
wait4 35.7%, recvmsg+accept+sendmsg (socket/RPC) 30.6%, select+epoll 14.3%, clone 6.6%. Dominated by
**blocking wait** (parents waiting on children, threads on RPC replies), latency-hidden under `-j8` — same as
perf#21a, consistent with perf#19 (blocking is not the wall bottleneck). Total fell 62.77→48.43 s (the
clone/fork syscall cost shrank with the fdtable). Notable counts: `getcpu` 8.4M, `recvmsg` 10.4M, `lstat`
657K, `mmap` 256K, `openat` 68K, `newfstatat` 72K.

## Ranked top bottlenecks by total wall/on-CPU contribution (post-D21b)
1. **Real LLVM (clang DSO) — ~35% userspace**, per-function unresolved (no PMU, no symbols).
2. **dyld — ~21% userspace** (per-exec bind + library mapping); its mapping work also feeds the kernel
   pagefault bucket.
3. **mmap/pagefault/pagetable — ~12.8% kernel** (per-process VM teardown on exit + fork pte-copy + dyld
   fault-in). Same root as (2): per-process loader/mapping/lifecycle.
4. libsystem (kernel-thunk + platform + malloc) — ~35% userspace combined.
5. socket/RPC receive — ~7.8% kernel (healthy, ring counters zero).
6. fd-table — 0.8% (**fixed**, was the #1).

## VERDICT: **A — dyld / pagefault / startup is the next lever.**
With the fork fd-table copy eliminated, the largest *attributable, addressable* cost is now the
**per-process loader + address-space churn**: dyld at 21% userspace (the biggest single userspace bucket
after clang, and abnormally large for a compiler) plus the ~12.8% kernel mmap/pagefault it drives (library
fault-in + VM teardown across hundreds of short-lived processes). clang's own ~35% is real LLVM CPU we can't
honestly chase without a PMU or a version-matched baseline (and it's not the Darling-specific cost). Buckets
B (syscall thunk 2.9% on-CPU), C (fs metadata — small on-CPU), and D (malloc 9% userspace, secondary) are all
smaller. Not F (STOP): there is a clear, non-LLVM, Darling-side lever left.

### Recommended next bead (perf#22): dyld / startup-mapping attribution
Profile **dyld's per-exec cost** (symbol bind, library mmap, page fault-in) under the emulation, and the
process-teardown mmap cost — e.g. which libraries are mapped+faulted per cc1, whether dyld shared-cache /
mapping reuse across the -j8 fan-out is possible, and whether the teardown `zap_pte_range` is reducible.
Recon-first (attribution), same discipline as perf#21a/b, no optimization until measured.

### Explicitly NOT touched (per scope)
NOFILE virtual cap (D21c), ring, psynch, dyld optimization (this is measure-only), no code changes. dup_fd
fix (perf#21b) confirmed durable and not regressed.

## Method / reproduce
Compact-fd mldr `f0cd2a82` live + original emulation dylib; warm shellspawn; `time` ×3 for wall/user/sys;
`sudo perf stat -a` for software counters (no PMU on this virtualized host — no IPC); `sudo perf record -a -g`
+ `perf report --comms=mldr --percentage=relative` (guest Mach-O userspace symbols don't resolve — DSO +
kernel only; the decisive mm bucket is kernel symbols so it resolves); `p21a-syscall.bt` for syscall
count/time; `/proc/<pid>/fd` for the no-1048575 check. Prod restored/untouched.
