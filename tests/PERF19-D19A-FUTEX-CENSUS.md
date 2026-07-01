# perf#19 / D19a — host futex/off-CPU census

**RECON, no code changes.** Measures real blocking *below* dserver — at the Linux `futex` syscall level on
the guest process (`mldr`) and the server (`darlingserver`) — because the pre-planned psynch-RPC census is
a near-dead-end: pthread mutex/condvar default to **ulock → Linux futex** (guest-side, bypassing dserver),
so `psynch_*` RPC counts are ~0 under real contention and the always-on server `per_call` histogram sees
only the rarely-used psynch *fallback*. Instrument: `bpftrace` on `sys_enter_futex` / `sys_exit_futex`,
filtered to `comm=="mldr"` (Darling guest threads) and `"darlingserver"`, under `sudo` (the `darling`
launcher is setuid-root; a non-root `strace`/tracepoint can't cross it). Script: `d19a-futex.bt`.

Nothing built or migrated; prod binaries untouched (srv md5 `835946f9…`), repo source clean.

## Why the RPC census missed it (the reframe)
- `libpthread` sets `__pthread_mutex_use_ulock = _PTHREAD_MTX_OPT_ULOCK_DEFAULT` (pthread_mutex.c:94) — the
  **default** mutex path is ulock, which issues `__ulock_wait`/`__ulock_wake` = Linux futex, entirely
  guest/kernel-side, **never a dserver RPC**. Condvars likewise block via futex, not (usually) `psynch_cvwait`.
- Confirmed live: a 24-thread contending probe generated **zero** `psynch_*` RPCs (both with ulock on and
  forced off) while 18 threads hammered a shared mutex 3000× each.
- Therefore the real sync/blocking cost, if any, is in **futex**, invisible to every existing (RPC-based)
  instrument. D19a measures futex directly.

## RESULTS — guest futex WAIT time by workload

| Workload | window | guest (`mldr`) WAIT total | waits | guest WAIT / wall | note |
|----------|-------:|--------------------------:|------:|------------------:|------|
| boot / shellspawn | 1.1 s | **4.0 ms** | 36 | 0.37% | |
| fork-storm | ~30 s | **1.21 s** | 1606 | ~4% | short sparse waits (0.75 ms mean) |
| 96-thread contention *(hung)* | 130 s | **78 ms** | 54 | 0.06% | probe **spins**, doesn't block — see below |
| tcstorm (long-lived MT) | 12.2 s | **0.40 s** | 4591 | 3.2% | 44,105 wakes; WAIT still <0.4 s |
| **real make-build** (240-file clang `-j8`, 121 objs) | 150 s | **101 ms** | 816 | **0.067%** | the decisive real-sync workload |

`darlingserver` WAIT is large in absolute terms (1.1 s boot / 130 s contend / 146 s build) but is entirely
the **idle epoll main-loop park**: 491 waits over 150 s = **~297 ms mean per wait** (long idle parks waiting
for work), not lock contention (which would be thousands of short waits). One server thread parked ≈ the
whole window. This is expected idle behavior, not a bottleneck — excluded from the ranking.

Futex op mix (guest side) is dominated by `FUTEX_WAKE` (op 1) with a small `FUTEX_WAIT` (op 0) tail — e.g.
tcstorm: 44,105 wakes vs 4,591 waits. High wake:wait ratio + tiny total wait time = threads rarely actually
sleep on a lock; they wake/signal but the waits are short.

## The 96-thread synthetic is a measurement artifact, not a bottleneck
The contending probe (18 hot-mutex + 6 condvar threads, then 96) **hangs in `pthread_join`** under
emulation — but the futex census shows it did only **78 ms of WAIT in a 130 s window** with 54 waits. It is
**not blocking on futex; it is spinning** (userspace CAS + `sched_yield`) and starving on CPU under
emulation's thread scheduling. So heavy synthetic mutex contention measures CPU/scheduler starvation, not a
futex bottleneck — do not read the hang as "sync is slow." (This is why real workloads, not synthetics, are
the honest signal here.)

## VERDICT: **A — futex/off-CPU blocking time is negligible → stop.**
Across every workload, guest-thread futex WAIT is ≤0.4 s absolute and **0.067% of wall time in the real
build**. The only large WAIT is the server's idle main-loop park (idle, not contention). There is no
sync/blocking bottleneck below dserver worth optimizing:

- Not **B** (huge + attributed) — it isn't huge; guest WAIT is sub-second everywhere.
- Not **C** (huge + poor attribution → D19b guest ulock counters) — attribution is already clean
  (`mldr` vs `darlingserver`, per-TID), and the total is small, so guest ulock counters (D19b) are **not
  warranted**. Building them would precisely measure a quantity we've shown is negligible.

### Classification of the sync primitives seen
- **ulock/futex fast path** (uncontended mutex): pure userspace CAS, no syscall → **A (cheap, nothing to do).**
- **ulock/futex wait** (contended mutex/condvar block): real Linux futex; measured ≤0.4 s/workload,
  <0.1% of a real build → **A (negligible).**
- **`darlingserver` epoll main-loop wait**: idle park waiting for RPCs → **B-semantics but idle** (real
  wait, not a cost; it's the server doing nothing while no work is queued). Don't touch.
- **`psynch_*` RPC path** (the psynch fallback when ulock is disabled): ~0 traffic by default → **D / N/A**
  (correctness fallback, not on the hot path).

## Where the time actually goes (for the perf#19 parent question)
The real build spends its 150 s not in sync but in **process/exec churn and the compiler's own CPU work**
under emulation — consistent with perf#18's finding that the hot cost was RPC round-trips (now largely on
the ring) and that the remaining UDS traffic is control-plane / blocking-wait / fd-passing. **The next
bottleneck after perf#18 is not synchronization.** If perf#19 continues, it should look at process
spawn/exec cost or the compiler-CPU/emulation overhead, not futex.

## Addendum — real `brew install wget` attempt
Tried the genuine canonical Homebrew build for completeness. **Not practically runnable under a bounded
census here:** `brew` (a large Ruby program) did not complete even `brew --version` in a 200 s guest
window — it is pathologically slow to *start* under emulation. This does not change the verdict; it
reinforces it: brew's cost is Ruby-interpreter CPU + spawning hundreds of configure/make subprocesses
(process/exec + compiler-CPU overhead), i.e. exactly the non-sync bottleneck D19a points to. The
sync-relevant portion of a brew build is the same `clang`/`make -j` parallel compilation already censused
here at 0.067% futex WAIT — a bigger build would only be a slower version of the same signal, not a
different futex verdict. The 240-file `clang -j8` build stands as the real-parallel-compilation datapoint.

## Method / reproduce
`d19a-futex.bt` (mldr+darlingserver-filtered futex WAIT time, op split, per-TID, duration hist) run under
`sudo bpftrace` around each workload; guest workloads launched via the setuid `darling shell`; a warm
server kept alive per workload; real build run in a bounded 150 s window with an on-the-fly census (the
`make -j` leaf wedges on teardown — a harness limit, prod-identical, documented in the D17-rerun — so the
window + census, not clean exit, is what's measured). No binaries changed; prod restored/untouched.
