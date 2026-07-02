# perf#25a — `brew reinstall` lifecycle census (launch-churn attribution)

Status: **DONE as recon — with a hard finding.** Measured on the CURRENT fixed deploy
(dyld 79b22273 + dar-gwn fixes, launcher setuid-root, `DPREFIX=darling-prefix-homebrew-test`),
DCC OFF. Reuses `p20a-lifecycle.bt` + the perf#24g exit_mmap teardown bucket.

## TL;DR
1. **`brew reinstall xz` is a genuinely launch/teardown-heavy workload** — **3627 processes**, 79 %
   of them tiny (<32 ms), 35 % never meaningfully on-CPU. This is exactly the class branch A
   (init/launch) targets, unlike the CPU-bound clang build (500 procs, perf#24g).
2. **BUT it reproducibly HANGS on a lost darlingserver RPC** before finishing — twice, at different
   stages. The linker `ld` (and all its threads) blocks in `recvmsg`/`__skb_wait_for_more_packets`
   on the dserver socket during the `make check` test-link; the whole `make → sh → clang → ld`
   tree parks behind it. **This is a correctness blocker that gates any launch-heavy perf work on
   brew — it must be fixed before a clean end-to-end brew A/B is even possible.**

## Method
`sudo bpftrace brew_census.bt` (per-binary wall/on-CPU via mldr `argv[1]`, spawn/clone/wait4, fs
churn, exit_mmap latency + unmap_vmas/zap_pte_range, all `comm=="mldr"`), then
`brew reinstall xz` under a spawn-progress watchdog. Guest killed when the tree deadlocked; bpftrace
flushed partial-but-rich census. `brew --version` smoke works instantly (ruby starts fine); only the
full `reinstall` deadlocks. Two independent runs both hung (run 1: single ruby stuck in
`__skb_wait_for_more_packets` early; run 2: 22-proc `make check` tree stuck behind `ld` recvmsg).

## Census (partial — up to the make-check hang; daemon rows = kill-time noise, excluded)
- **Processes: 3627.** spawns 4797, clone 7488, **wait4 84 647**, openat 458 248, newfstatat 283 187.
- **Per-proc lifetime** (clean, 3611 procs): **79 % ≤ 32 ms**, mass in 2–32 ms — tiny short-lived.
- **Per-proc on-CPU %**: **1260 procs (35 %) at 0 % on-CPU** — never ran meaningfully; pure
  spawn+loader+teardown. Real workers cluster at 32–128 %.
- **mm-teardown (the DCC6-addressable bucket): exit_mmap 12 283 calls, 3.642 s total, mean 296
  µs/exit**; hist mass 256–512 µs. (12 283 vs 3627 procs ≈ several exit_mmap per proc — threads.)
- **Per-binary cumulative wall (real workers, hung daemons excluded):** `/bin/sh` ~1.01 s ·
  `make` ~0.95 s · `clang` ~0.35 s · `ld` ~0.31 s · this-prefix `bash` ~29 ms · `sed` ~6 ms ·
  ruby ~3–16 ms each. Utility swarm (grep/awk/tr/sleep/sort/dirname/mkdir/expr/…) each 0.1–0.4 s
  cumulative across hundreds of invocations.

## What this says about the two branches
- **The shape confirms branch A's premise:** brew is dominated by *thousands of tiny processes*,
  not by a few CPU-bound compiles. 35 % of procs never touch the CPU — their entire cost is
  fork+exec+dyld-load+init+teardown. That is precisely the bucket DCC6 (VMA/teardown) and libSystem
  init-laziness would attack. So a launch-heavy workload where DCC6's win *would* show is real and
  brew is a valid target — consistent with the perf#24 FINAL conclusion (DCC6 = launch-churn win).
- **But the RPC hang dominates everything.** No launch/init micro-optimization matters if
  `reinstall` can't complete. The honest ordering for branch A is now:
  **(A0) fix the lost-RPC hang in the linker/`make check` path FIRST**, then
  **(A1) DCC6-on brew A/B** (does the 3.64 s teardown bucket shrink like perf#24g predicts?), then
  **(A2) libSystem init-laziness** (the residual per-proc init the 1260 zero-CPU procs still pay).

## The hang (evidence, not a fix — out of scope per instruction)
- Run 2, at `make check`: `ld` host pid, 4 threads all `recvmsg(fd 8188..8191, <unfinished>)` on
  dserver sockets; wchan `__skb_wait_for_more_packets`. Parents `make`(×3)/`sh`(×5)/`clang` all in
  `do_wait` behind it; one `Z` zombie unreaped. Real work (configure + compile) had already
  succeeded — the deadlock is a **dropped/lost dserver RPC reply to the linker**, same
  `__skb_wait_for_more_packets` class as run 1's early ruby stall.
- This matches the known perf#18/ring residual-RPC-loss failure mode under heavy concurrent load
  (`__skb_wait_for_more_packets` = waiting on a reply that never arrives). NOT chased here.

## Boundary / status
- Recon + measurement only. No code. Prod already at baseline (dyld 79b22273 both / dserver
  835946f9 / mldr f0cd2a82) — this run used the fixed deploy with DCC OFF, nothing to restore.
  Guest torn down clean. bpftrace census saved: `brew_census_full.out` (job tmp).
- **Next (branch A), in order:** A0 lost-RPC-hang fix (blocker) → A1 DCC6-on brew A/B →
  A2 libSystem init-laziness audit. Do not start A1/A2 before A0.
