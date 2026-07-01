# perf#21a — cc1 / clang CPU attribution under emulation

**RECON, no code changes.** perf#20a showed clang is ~79% of real build **wall**, and clang runs ~40% on-CPU
/ 60% off-CPU. perf#21a answers the follow-up: **within that clang time, is it real LLVM work or
Darling/runtime overhead?** Option-1 method (no native/Docker baseline — the question is *where inside the
Darling-run* time goes, an attribution question, not a "how much slower than ideal clang" question).

Prod binaries untouched (srv md5 `835946f9…`); no kernel sysctl changed (used per-command `sudo perf`, not a
persistent paranoid/kptr relaxation); source read-only. Workload = the perf#20a 200-file clang `-j8` build.

## Measurement constraints found
- **No PMU.** `cycles`/`instructions`/`branches` = `<not supported>` — virtualized host, no hardware counters.
  So **no IPC**. Software counters (cpu-clock, context-switches, cpu-migrations, page-faults) work.
- **Guest Mach-O symbols do NOT resolve in perf** — userspace guest frames show as bare addresses
  (`0x100012bae`). DSO-level attribution resolves (perf reads the load map); per-*function* userspace
  attribution would need a symbols/unwind bead. Kernel symbols resolve fine (via `sudo perf`).

So attribution is **DSO-level for userspace + full-symbol for kernel** — which turned out to be enough,
because the decisive cost is a *kernel* symbol.

## RESULT 1 — build CPU is kernel-heavy
Single 200-file `-j8` build, `time`: **wall 1.73 s, user 3.08 s, sys 5.72 s.** ~**65% of CPU time is in the
kernel**, not userspace. `perf stat` (system-wide, one build): 773,381 page-faults (41 K/s), 54,991
context-switches. (Note: perf#20a's "13.95 s" was Σ-of-per-proc-lifetimes; wall with -j8 parallelism is ~1.7 s.
Both correct — Σ-lifetime is the work proxy, wall is elapsed.)

## RESULT 2 — DSO self-time (comm=mldr, on-CPU samples, relative)
| DSO | self | what it is |
|-----|-----:|------------|
| clang | 37.2% | real LLVM frontend/backend |
| **dyld** | **21.4%** | dynamic loader (per-exec symbol bind + mapping) |
| libsystem_kernel | 14.6% | syscall thunk layer |
| libsystem_platform | 9.2% | platform primitives |
| libsystem_malloc | 8.6% | allocator |
| libobjc / libdyld / libsystem_c / pthread / libc++ | <2.2% each | — |

**clang (real LLVM) is only 37% of guest on-CPU time.** The build is NOT dominated by real compiler work —
~55%+ is loader + libsystem + allocator, i.e. Darling-runtime-side. dyld at 21% (a *fifth* of compiler
on-CPU) is abnormal for a compiler and flags per-process loader/mapping cost.

## RESULT 3 — the #1 on-CPU symbol is a kernel fork cost (the finding)
Top guest on-CPU symbols (self, kernel resolved, userspace `[.]` = unresolved Mach-O):

| symbol | self | |
|--------|-----:|--|
| **`[k] dup_fd`** | **13.30%** | **copy fd-table on fork/clone** |
| `[k] next_uptodate_folio` / `filemap_map_pages` / `do_user_addr_fault` | ~5.6% | page-fault / mmap (dyld mapping) |
| `[k] ____sys_recvmsg` / `unix_dgram_recvmsg` / `apparmor_socket_recvmsg` | ~2.5% | UDS RPC receive |
| `[k] vma_interval_tree_insert` / `_copy_from_user` / spinlocks | — | fork/mmap/copy |
| `[.]` unresolved Mach-O userspace | ~half of userspace | (LLVM, unattributable per-fn) |

### Root cause of `dup_fd` = 13%: Darling places control fds at the TOP of the fd space
Every guest process holds a **socket at fd number 1,048,575** (= `RLIMIT_NOFILE − 1`; confirmed present in
117/184 live guest procs, target = a dserver/UDS `socket:[…]`). Source: `mldr.c:601,625` — Darling's fd
bitmap sets `highest = rlim_cur − 1` and allocates its internal control fds (dserver socket, lifetime pipe,
eventfds) **downward from the top** (`fd = highest − next_index`), deliberately to keep Darling's own fds
clear of the guest program's low-numbered fds (macOS software hardcodes low fds).

**Consequence:** one fd at index 1,048,575 forces the kernel fd table to span **1,048,576 slots ≈ 8.4 MB**
(8 MB `struct file*` array + ~0.4 MB bitmaps). Every `fork()`/`clone()` then does an **~8.4 MB `alloc_fdtable`
→ `__vmalloc_node_range` → copy in `dup_fd`** — to carry 3–40 *actually-used* fds. Across a 200-file build
(≈419 forks): **~3.4 GB of fd-table copy churn**, surfacing as the single hottest symbol at 13.3% of guest
on-CPU. Callgraph confirms `dup_fd → alloc_fdtable → __vmalloc_node_range → alloc_pages`.

## RESULT 4 — syscall time is mostly blocking wait (not a CPU bucket)
bpftrace per-syscall *time* (comm=mldr, 3 builds): recvmsg 28% + accept 5% + sendmsg (socket/RPC) ≈ 34%,
wait4 27%, select 12% + epoll_wait 8% (poll waits), clone 6.6%. **These are dominated by blocking/off-CPU**
(threads sleeping on RPC replies / child waits), latency-hidden under `-j8`, and consistent with perf#19's
verdict that blocking is not the wall bottleneck. They are listed for completeness, **not** as a CPU bucket —
the on-CPU truth is RESULT 2/3.

## Ranked total-CPU buckets (guest on-CPU)
1. **Darling process-model / fork fd-table copy — ~13% (single symbol `dup_fd`), the clear #1 lever.**
2. Real LLVM (clang DSO) — ~37% userspace, but per-function unresolved.
3. dyld loader (bind + mapping) — ~21% userspace + page-fault kernel tail (~5.6%).
4. libsystem (kernel-thunk + platform + malloc) — ~32% userspace.
5. Filesystem metadata — small (openat/stat ~6% of *syscall time*, ~1% on-CPU).
6. Unresolved userspace Mach-O — ~half of userspace samples (bucket E for per-function detail).

## VERDICT: **B — Darling-runtime overhead is real and has one concrete, high-value lever: the fork fd-table copy.**
Not A (real LLVM is only ~37%, and even that is inflated by the driver-clang's own loader cost). The single
most actionable, highest-confidence win is **RESULT 3**: the top-of-fd-space control-fd placement makes every
fork pay an ~8.4 MB fd-table `vmalloc`+copy for a handful of real fds — **13% of guest on-CPU, ~3.4 GB of
churn per build**, on the hottest path of a fork-heavy workload (every compile spawns driver+cc1).

## Recommendation — exactly one next lever
**perf#21b (or a fork-cost bead): stop forcing a 1 M-slot fd table.** Options to evaluate (design-only, not in
this bead):
- Place Darling's internal control fds **low and packed** (just above the guest's expected range) instead of
  at `RLIMIT_NOFILE − 1`, OR
- Lower the guest `RLIMIT_NOFILE` to a sane cap (e.g. 4096/65536) so `highest` — and thus the fd table —
  is small, OR
- Use `O_CLOEXEC` + close-on-fork semantics / a compact reserved band so `dup_fd` copies a small table.

Any of these should remove ~13% of guest on-CPU on real builds — the largest single attributable Darling
overhead found. **Validate with the same perf#21a instruments** (expect `dup_fd` to fall out of the top and
sys-time to drop) plus perf#20a's wall/lifetime census, and a correctness pass (macOS low-fd assumptions must
still hold — this is why the high fds exist; the fix must preserve that invariant).

### Explicitly NOT next
- Not more ring/opcode/psynch/duplex (perf#18 done; RPC on-CPU here is ~2.5% kernel recv, healthy).
- Not sync/futex (perf#19 verdict A; futex is 0.5% of syscall time).
- Not "optimize clang" — real LLVM is only ~37% and we have no PMU/native baseline to chase it honestly;
  chasing it would need a symbols/unwind bead (E) first, and it's not the top lever anyway.
- A **dyld/loader startup bead** (21%) is the plausible *second* lever after the fd-table fix, but see
  perf#20a: it mostly matters for shell/fork-heavy trees; measure again after the fork fix.

## Method / reproduce
`sudo perf record -a -g` around the build (kernel symbols resolve; guest Mach-O userspace does not) →
`perf report --comms=mldr --percentage=relative` for DSO + kernel-symbol self-time;
`sudo perf stat -a` for software counters (no PMU); `p21a-syscall.bt` (bpftrace raw_syscalls, comm=mldr) for
per-syscall count/time; live `/proc/<pid>/fd` inspection for the fd-1048575 confirmation. Warm server (PID
constant), `timeout -s KILL`, no binaries changed, prod restored/untouched. Artifacts:
`p21a-dso-breakdown.txt`, `p21a-symbol-selftime.txt`, `p21a-perf-stat.txt`, `p21a-syscall.bt`.
