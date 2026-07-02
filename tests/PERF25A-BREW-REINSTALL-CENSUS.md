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

## UPDATE — hang root-classed (perf#25a-hang / bead #111)
The hang is NOT missing homebrew psynch fixes, and NOT (only) the experimental ring fast-path:
- **psynch fixes ARE present.** Deployed xnu HEAD `caddc2b` (perf/shmem-ring-guest) has BOTH
  `psynch: fix cvsignal/cvbroad argument widths` (8a7797f) and `return psynch wait errors as
  negative errno` (f7ae4e0) as ancestors; source `psynch_cvwait.c` materially returns `-ret`/`-EINTR`.
  The dar-q95.2/.19 download-hang fixes are IN. So the earlier "missing homebrew patch" hypothesis is
  ruled out.
- **Ring fast-path OFF does NOT fix it.** Re-ran `brew reinstall xz` with `DARLING_SERVER_FAST_OPS=0`
  (ring inline fast paths disabled). Still deadlocks — same `__skb_wait_for_more_packets` — but on a
  different process: `sh ../../libtool --mode=compile clang …` (compile stage) instead of `ld`
  (make-check stage). Frozen set stable 20 s (23 procs).
- **Real class = stranded SIGCHLD / lost wait4 reply.** The stuck `sh` has a **zombie child**
  (Z, ppid == the stuck sh): the child exited but the parent is parked forever in
  `__skb_wait_for_more_packets` instead of being woken to reap it. wchan = waiting on a dserver RPC
  reply (the child-exit / wait notification path) that never arrives. This is DEEPER than the ring
  fast-path — it's darlingserver's wait/SIGCHLD delivery under heavy concurrent fork/exec/exit.
- **Consequence:** a clean end-to-end brew reinstall A/B is blocked until this stranded-wait bug is
  fixed. This IS the (A0) blocker for branch A, above any launch/init micro-optimization. Recon only,
  not chased here.

## UPDATE 2 — ROOT CAUSE FOUND (A0 / dar-gwn.1.7 family / bead dar-dar6x4-perf-5dq.15)
The hang is a **ring↔UDS reply-channel split-brain** in the perf#18 shared-memory ring transport.
Not psynch, not reaping-loss primary (the zombie is a downstream consequence).

REPRODUCED + A/B (deployed baseline server 835946f9, guest dylib built with DARLING_RING_TRANSPORT=ON):
- Ring ON  (default): `brew reinstall xz` HANGS intermittently (~1 in 2–3). Leaf `sh ../../libtool
  --mode=compile clang …` parked in `__unix_dgram_recvmsg` (UDS RPC recvmsg); every make/sh ancestor
  correctly in `do_wait` behind it; server fully IDLE (`ep_poll`, `clients_blocked_in_rpc:0`,
  `workqueue_depth:0`). The server believes it replied; the guest never got it.
- Ring OFF (`DARLING_SERVER_FAST_OPS=0`): `brew reinstall xz` COMPLETES cleanly (🍺 ~53–54 s), 2/2.

MECHANISM (code-level, both sides):
- GUEST `gr_wait_reply()` (dserver-ring.c:420): after 512 spin iters it arms `s2c_waiters=1` and enters
  a **bounded** futex-wait loop `for (guard=0; guard<100000; ++guard)`. Each `FUTEX_WAIT` returns EINTR
  on every signal. Under a make -jN **SIGCHLD storm**, 100000 EINTR cycles burn in well under a second,
  so the loop **exhausts and returns NULL → the caller UDS-falls-back** and blocks on `recvmsg`.
- SERVER `Thread::_publishReplyToRingLocked()` (thread.cpp:2213) via `pushCallReply` (2249): the reply
  channel is chosen SOLELY by `_ringReplyPending` (armed by `beginRingReply` when the request arrived
  over the ring) — NOT by whether the guest is still listening on the ring. So the server publishes the
  reply onto the **s2c ring** + `wakeGuest()` while the guest has already abandoned the ring and is on
  **UDS**. Reply lands in a buffer nobody reads; UDS resend gets no matching reply. Permanent deadlock,
  server idle. (The guest comment at dserver-ring.c:482 already flags "the reply may still land" — benign
  only for the very first op; under load a mid-stream op hits it and wedges.)

WHY ONLY UNDER BREW (not the synthetic `make -j8` clang harness): brew's configure/libtool/`make check`
drive a far denser SIGCHLD flood (thousands of nested `sh -c` + short tools + test-runs), which is what
pushes the 100000-guard to exhaust. Flat `( ) &`+`wait` fork storms and a plain `%.o` Makefile do NOT
reproduce (3/3 + 3200-fork storm clean).

FIX DIRECTION (option 1, surgical): once the guest has published a request the server will answer on the
ring, the ring is the ONLY channel that reply will ever arrive on — so the guest must NOT abandon it.
Make the slow-path wait for a ring-originated reply effectively unbounded (keep EINTR/EAGAIN re-loop; a
genuine server death is already globally fatal, so liveness is unchanged). Removing the arbitrary 100000
bound closes the split-brain. Alternatives: (2) teach the server to fall back to UDS when the guest
abandoned (invasive, needs a guest→server "I left the ring" signal); (3) ship DARLING_RING_TRANSPORT=OFF
by default (the experiment is not supposed to be default-on per its own patch blocker; the deployed tree
has it ON). Rate + fix to be validated by the A0 A/B batch + a rebuilt guest dylib.

## UPDATE 3 — option-1 fix REGRESSED BOOT; the UDS fallback is LOAD-BEARING (A0 still open)
Built option 1 (guest `gr_wait_reply`: `for (guard<100000)` -> `for(;;)`), libsystem_kernel a6608de1 from
~/work/darling-build, deployed to all 3 copies (baseline 6bd251c3 backed up first).
RESULT: **boot HANGS** — launchd never brings up shellspawn.sock; guest wedges during early init.
=> The premise "the server ALWAYS replies on the ring for a ring-originated op, so the guest can wait
forever" is FALSE. There is at least one boot-path case where a `_ringReplyPending` op's reply arrives
over UDS (or the guest legitimately must abandon the ring and retry over UDS). Server-side candidates: the
`_interruptedForSignal` saved-reply branch and the `_deferReplyForS2C` deferred-reply branch in
`pushCallReply` (thread.cpp:2256/2262) bypass/delay `_publishReplyToRingLocked`, so a signal or S2C upcall
on a ring op can route its reply off the ring. The OLD bounded guard + UDS fallback is what let boot
survive that — the fallback is LOAD-BEARING, not merely a bug escape.
REVERTED byte-identical to baseline 6bd251c3 (all 3 copies); `west darling-doctor` ALL GREEN; brew ring-OFF
reconfirmed 🍺 clean; guest torn down; fix branch discarded (source back on perf/shmem-ring-guest @ caddc2b).

CORRECTED FIX SHAPE (A0, still to build): close the split-brain SYMMETRICALLY, do NOT remove the fallback.
When the guest abandons the ring and UDS-falls-back, either (a) the SERVER must be told to reply on UDS
(clear `_ringReplyPending` / re-point the sink), or (b) the guest must drain a late ring reply before
UDS-waiting, or (c) the server replies on BOTH channels for the abandonment window. This is genuine
guest+server co-designed transport correctness on the EXPERIMENTAL ring — not a one-line change. Every
candidate MUST pass BOTH a boot smoke AND the brew ring-ON A/B (RED baseline: RING_ON 3/3 hang, RING_OFF
2/2 clean) before any deploy. Safe interim posture: `DARLING_SERVER_FAST_OPS=0` (proven clean) or building
the guest dylib with `DARLING_RING_TRANSPORT=OFF`.
