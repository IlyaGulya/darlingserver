# perf#21b — compact protected internal fd band

Fixes the perf#21a #1 lever: the fork-time `dup_fd()` cost from a 1M-slot kernel fd table. **Implementation +
live validation** (not recon). Source change is in `src/startup/mldr/mldr.c` (top-level darling repo, local
commit); the emulation `getrlimit`/`setrlimit` are intentionally left at their compatible behaviour (see
"Deferred" below).

## Problem (from perf#21a)
Darling's internal service fds (per-thread dserver RPC sockets, the process lifetime pipe) were allocated
**downward from `RLIMIT_NOFILE - 1`**. darlingserver raises `RLIMIT_NOFILE` to `/proc/sys/fs/nr_open`
(=1048576) and guests inherit it, so a single service fd landed at **fd 1048575**. That forces the Linux
kernel to size the process fd table to **1,048,576 slots (~8.4 MB)**. Every `fork()`/`clone()` copies the
whole table in `dup_fd()` → perf#21a measured **13.30% of guest on-CPU** (the #1 symbol), ~3.4 GB of
fd-table copy churn per 200-file build, to carry 3–40 actually-used fds.

## Change
Anchor the service-fd band at a **compact ceiling** instead of the top of fd space:
- `DARLING_INTERNAL_FD_TOP` — host fd ceiling (exclusive), default **8192**, env-overridable for experiments.
- `DARLING_INTERNAL_FD_BAND` = 512 — band width; band = `[top - 512, top)`.
- `mldr` sets the process **soft `RLIMIT_NOFILE` to exactly `top`** on first service-fd allocation, in
  **either direction** — the inherited soft limit may be higher (nr_open) or lower (launchd's restored
  default). Setting it up as well as down fixes an intermittent **"Failed to create socket"** seen when a
  process inherited a soft limit below `top` (the old code only ever used `rlim_cur-1`, so it never needed to
  raise; hardcoding `top` does).
- Service fds are still allocated downward from `top-1`, still protected by the guard table.

Because the soft limit == `top` and the highest live fd is `top-1`, the kernel fd table spans only ~`top`
slots, so the per-fork `dup_fd()` copy is tiny.

## Acceptance results

**1. Live fd map** — PASS. Every guest proc: `max_fd = 8191`, `soft rlimit = 8192` (was 1048575 / 1048576).
No service fd at 1048575 (checked all live procs); no fd ≥ 8192; service fds observed at 8189/8190/8191,
inside the band `[7680, 8192)`.

**2. Guest limit (strict virtual cap)** — **DEFERRED to D21c** (see below). getrlimit still reports the
historical `real - 1`.

**3. Correctness** — PASS. Boot clean (7 daemons, same as the original-binary baseline; the pre-fix build
booted only 1). shellspawn comes up. 200-file clang `-j8` build → **200/200 objs**. fork-storm (48 children) →
clean. Guest `close`/`dup`/`dup2`/`fcntl` against the service-fd region are guarded (guest sees them as
inaccessible/EBADF, the live sockets survive, the guest stays alive). Ring health counters all zero
(`ring_fast_fail`/`ring_s2c_full`/`ring_fast_suspend`/`clients_blocked_in_rpc` = 0 across 49K RPCs / 1240
forks). No fresh fd/socket errors or faults in dserver.log.

**4. Perf** — PASS, the headline. Re-running the perf#21a census (200-file clang `-j8`, `sudo perf record -a`,
`--comms=mldr` relative self-time):

| symbol | perf#21a (before) | perf#21b (after) |
|--------|------------------:|-----------------:|
| `[k] dup_fd` | **13.30% (#1)** | **0.09%** |

`dup_fd` fell ~148× and out of the top of the profile; the kernel fd table is ~8192 slots instead of ~1M.

**Band-base sweep** — PASS at `DARLING_INTERNAL_FD_TOP` = **4096 / 8192 / 16384**: each boots 7 daemons with
`max_fd = top-1`, `rlimit = top`. 8192 is the default — comfortable headroom over typical guest fd usage while
keeping the table ~128× smaller than the old 1M.

## Deferred to D21c — strict guest NOFILE cap
The task's acceptance item 2 (guest `getrlimit(NOFILE)` reports *below* the band; guest `open`-until-EMFILE
stops before the band; guest can never allocate a descriptor number inside the band) was prototyped by
changing the emulation's `getrlimit`/`setrlimit` to subtract/add the whole band (report `top - BAND` instead
of `real - 1`). **It broke boot**: with the band-hiding report, the guest booted only 1 daemon and shellspawn
never started. Isolated by A/B (new-mldr + original-dylib boots 7 daemons; new-mldr + band-hiding-dylib boots
1). The break was **not** the band magnitude (BAND=1 also failed) and **not** `setrlimit` (never called during
boot). Root cause: some guest daemon misbehaves when `NOFILE` is reported below the real ceiling — deferred to
**D21c** as a compatibility investigation.

Until D21c, the emulation keeps the compatible `real -/+ 1` behaviour, so the collision surface is **identical
to the pre-perf#21b design**: guest cap == `top-1` == the highest service fd. Internal fds remain protected by
the **guard table** (`guard_flag_prevent_close` on `close`/`dup`/`dup2`/`fcntl`, `close_on_fork` on fork), not
by the reported limit — this is the same protection the old top-anchored scheme relied on.

## Method / reproduce
Build `mldr` + emulation dylib; deploy `mldr` to both prefix copies (`usr/libexec/darling/mldr` and
`libexec/darling/usr/libexec/darling/mldr`); keep the original emulation dylib. Boot via the setuid `darling
shell`, warm shellspawn, run the perf#20a/21a harnesses. `sudo perf record -a -g` + `perf report
--comms=mldr --percentage=relative` for the dup_fd measurement; `/proc/<pid>/fd` + `/proc/<pid>/limits` for
the fd map; `darling-stat` for ring/health counters. Backups of the original deployed `mldr`/dylib kept for
restore. Prod launcher prefix binaries updated in place for the live prefix; source committed locally, not
pushed.
