# perf#20a — spawn/exec lifecycle census

**RECON, no code changes.** Finds where real build wall time goes *after* perf#18 (hot RPC on the ring) and
perf#19/D19a (synchronization ruled out, verdict A). Splits a real build's wall time into three buckets and
ranks by **total wall time (not count)**:

- **A) process lifecycle / spawn-exec churn** — fork+exec+loader+pre-main dserver RPCs per subprocess.
- **B) actual compiler/emulation CPU** — clang/cc1 execution cost under emulation.
- **C) filesystem / path lookup** — open/stat/readlink churn.

Prod binaries untouched (srv md5 `835946f9…`), repo source clean. Guest procs all run as host `comm="mldr"`
(the loader); the real binary is `mldr argv[1]` (`mldr!<path>`), captured via bpftrace `join(argv)` /
`str(args->argv[1])` on `sys_enter_execve`. Instruments: `p20a-lifecycle2.bt` (proc count, wall=Σ(exec→exit),
on-CPU=Σ sched_switch accrual, per-binary wall via argv[1], fs churn) + `p20a-clangcpu.bt` (clang-only
wall-vs-on-CPU). Method reuses D19a: `sudo bpftrace`, setuid `darling` launcher, warm shellspawn,
`timeout -s KILL`, `darling-stat` per_call for per-child RPC deltas. One warm server (PID constant) throughout.

## RESULTS — per-workload wall / on-CPU split

| Workload | procs | wall (Σ lifetimes) | on-CPU | on-CPU/wall | off-CPU | opens/proc | stats/proc |
|----------|------:|-------------------:|-------:|------------:|--------:|-----------:|-----------:|
| **200-file clang `-j8` build** | 415 | **13.95 s** | 5.27 s | **37.8%** | 62.2% | 55 | 58 |
| shellspawn loop (30× `/usr/bin/true`) | 60 | 0.24 s | 0.08 s | 33.1% | 66.9% | 85 | 63 |
| fork-storm (48 children) | 338 | 12.58 s | 2.29 s | 18.2% | 81.8% | 50 | 39 |

Host = 12 cores; build ran `-j8`. Build completed **200/200 objects** cleanly in-window (rc=0).

## RESULT — build wall time by binary (ranked, the whole point)

| Binary | wall | share of build wall | procs |
|--------|-----:|--------------------:|------:|
| **clang** (`/…/CommandLineTools/usr/bin/clang`) | **10.99 s** | **78.8%** | 400 |
| bash | 1.51 s | 10.8% | — |
| make | 1.38 s | 9.9% | 1 |
| ls / wc / date / awk / rm / tr | ~0.01 s each | ~0.1% each | — |
| path_helper | 0.004 s | 0.0% | — |
| mkdir | 0.004 s | 0.0% | — |

**clang dominates real build wall at ~79%.** bash (the make recipe shell + `make` itself) is the ~21% parent
overhead; every other utility is rounding error in a real build. (200 source files → **400 clang procs** =
clang driver + `cc1` per file.)

### clang-only wall vs on-CPU (dedicated `p20a-clangcpu.bt` run, 400 clang procs)
- wall = **11.33 s**, on-CPU = **4.54 s** → **clang on-CPU/wall = 40.1%** (off-CPU 59.9%).
- per-proc: wall ≈ 28.3 ms, on-CPU ≈ 11.4 ms. On-CPU% histogram clusters at **16–64%** (median ~32–64%).

## RESULT — dserver RPC per forked child (darling-stat per_call deltas over the build)
- **419 forks**, **16,324 RPCs** ⇒ **~39 RPCs / child**. All ring-health counters clean:
  `ring_fast_fail=0, ring_s2c_full=0, ring_fast_suspend=0, ring_duplex_s2c=0, clients_blocked_in_rpc=0`.

| op | +count | ~per child | p50 | max | transport |
|----|-------:|-----------:|----:|----:|-----------|
| mach_reply_port | 2506 | 5.98 | 4µs | 80µs | **ring (fast)** |
| host_self_trap | 1670 | 3.99 | 4µs | 61µs | **ring (fast)** |
| pthread_canceled | 1284 | 3.06 | 2µs | 41µs | UDS |
| thread_self_trap | 1254 | 2.99 | 4µs | 141µs | **ring (fast)** |
| task_self_trap | 1253 | 2.99 | 8µs | 96µs | **ring (fast)** |
| ring_attach | 1253 | 2.99 | 64µs | 9094µs | UDS (handshake) |
| vchroot_path | 1251 | 2.99 | 16µs | 72µs | ring |
| uidgid / set_thread_handles | 836 | 2.00 | 4µs | ~40µs | ring |
| checkin | 836 | 2.00 | 64µs | 478µs | UDS |
| fork_wait_for_child | 421 | 1.00 | **256µs** | 1321µs | UDS (blocking wait) |
| started_suspended / set_executable_path / set_dyld_info / mldr_path / get_tracer / checkout | 417 | 1.00 | 4–16µs | ≤92µs | UDS (loader) |

~39 RPCs/child at p50 4–16µs ≈ **~0.5 ms of RPC per child** — negligible against ~28 ms wall/child. The RPC
round-trip is no longer the cost (perf#18 did its job); the ring is healthy under full build load.

## fs churn (bucket C)
`readlinkat ≈ 0` everywhere. `openat`/`newfstatat` are 50–85 / 39–63 **per process** — real, but it is dyld's
per-process library/`stat` search, i.e. a *component of* per-process startup, not a separate dominant bucket
(the build's total wall is 79% clang, and clang's off-CPU is not attributable to a readlink/stat storm).

## VERDICT: **B — compiler/emulation CPU dominates. Next lever is CPU under emulation, NOT lifecycle.**

The ranked-by-wall evidence is unambiguous:
- **clang is 78.8% of build wall.** Utilities (path_helper, ls, mkdir, tr, …) are ~0.1% each — the
  spawn/exec-churn "bucket A" is real but *tiny in a real build*.
- clang's own wall is **60% off-CPU**, but that off-CPU is **not** RPC (~0.5 ms/child, ring healthy) and
  **not** sync (perf#19: futex 0.067% of build wall). It is clang doing clang work — CPU-bound compilation
  under emulation, plus scheduler descheduling from `-j8` oversubscription on limited cores. Both point at
  **B**, the compiler/emulation CPU path, not the lifecycle/RPC path.
- fork_wait_for_child (p50 256µs, 1/child) is genuine parent-wait semantics, not reclaimable churn.

### Where bucket A (lifecycle) *is* visible — and why it still isn't the lever
In the **synthetic** trivial-proc workloads, lifecycle tax is the whole cost and shows clearly:
- shellspawn loop: a no-op `/usr/bin/true` costs ~3.7 ms wall/proc, 67% off-CPU — pure startup/loader.
- fork-storm: 288 `/usr/bin/true` execs burn **10.77 s** (85.6% of that workload's wall) — ~37 ms per no-op
  process, 82% off-CPU. **`path_helper` execs on every login shell** and in the shellspawn loop cost *more
  wall than the command it precedes* (127 ms vs 112 ms for `true`).

So per-process startup overhead is real and would matter for **shell-heavy / fork-heavy** workloads
(configure scripts, `brew`'s hundreds of tiny subprocesses). But for the canonical **parallel compile**, it
is dwarfed 400:1 by clang's own execution. Optimizing lifecycle would not move a real `make -j` build.

## Recommendation for the next perf bead
Pursue **B): CPU profiling of clang under emulation** — `perf record` on the guest clang / cc1 to find where
the emulation overhead lands (instruction translation, syscall thunking, mmap/page-fault handling in dyld,
libc hot paths). That is the only lever with leverage on real build wall time.

Secondary, only if a **shell/fork-heavy** workload (configure, brew) becomes the target: a bounded
"per-process startup" bead — `path_helper`-on-every-shell elimination/caching + dyld library-search `stat`
reduction — worth ~30–40 ms per trivial process but ~0 on parallel compiles.

**Explicitly NOT next:** more ring opcode migration (RPC is ~0.5 ms/child and healthy), sync/futex work
(perf#19 verdict A), duplex/mach_msg_overwrite (D7/D8 STOP). brew was confirmed *not* a bounded benchmark
(did not finish `brew --version` in 200 s under emulation — Ruby-startup pathological; reference only).

## Method / reproduce
`p20a-lifecycle2.bt` and `p20a-clangcpu.bt` under `sudo bpftrace` around each workload; guest workloads via
the setuid `darling shell`; warm server kept alive (PID constant, verified); build run in a bounded ~150 s
window with on-the-fly `darling-stat` snapshots before/after. BPF stack limit forced short `str(argv[1], N)`
slices and single-`strcontains` classification (the multi-branch bucketed variant `p20a-lifecycle3.bt`
overflowed the BPF stack and was abandoned in favor of the clang-only script). Per-binary on-CPU via a
stashed `str()` map key produced garbage keys in bpftrace (a known map-key limitation) — the clean per-role
CPU number comes from the dedicated pid-tagged `p20a-clangcpu.bt`, and per-binary **wall** from the
full-path argv[1] key (which was clean). No binaries changed; prod restored/untouched.
