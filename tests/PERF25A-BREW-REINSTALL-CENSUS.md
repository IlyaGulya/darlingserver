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

## UPDATE 4 — attempt-2 fix also regressed; SOCKET-LEVEL capture reframes the bug (A0 still open)
Attempt 2 = symmetric: guest `gr_wait_reply` unbounded (`for(;;)`) + server InterruptExit saved-reply flush
routed through `_publishReplyToRingLocked` (mirror the S2C-defer flush). Built libsystem_kernel 7e311a9b +
darlingserver 196801fa from ~/work/darling-build, deployed 4 copies (baselines 6bd251c3 / 835946f9 backed up).
RESULT: brew ring-ON still hung (stall MOVED libtool-compile -> configure `ld` link), AND — decisively —
**ring-OFF (`FAST_OPS=0`), which is CLEAN on baseline, ALSO hung with this build** => the server-side
InterruptExit change is WRONG as written (routes a reply to a ring the interrupted guest is not reading).
Reverted byte-identical (dylib 6bd251c3 x3, srv 835946f9), doctor GREEN, both fix branches' edits discarded.

SOCKET-LEVEL CAPTURE (baseline binaries, `ss -x` on the hung guest — the reframing evidence, saved
tmp/a0_sock_snapshot.HANG.txt): the wedge is NOT a simple lost ring reply. At the hang, with the server IDLE
(ep_poll, workqueue_depth:0, clients_blocked_in_rpc:0), TWO socket anomalies coexist on the guest side:
  1. **darlingserver fd=91 has Send-Q = 768 bytes STUCK** (u_seq / SEQPACKET), peer = the stuck guest's fd
     (launchd fd=39 in the captured hang). The server produced a ~768B reply, handed it to the async writer
     (AsyncWriter: O_NONBLOCK + Writable-Monitor, async-writer.cpp), but it never drained — the peer's recv
     buffer never freed because the peer isn't reading.
  2. **guest launchd fd=14 has Recv-Q = 12 bytes UNREAD** — data sitting in a socket the guest owns while all
     its threads are parked in recvmsg on OTHER fds => a missed wakeup / wrong-fd wait in the guest's
     multi-socket RPC receive multiplexing.
So the true failure is a **guest multi-fd receive-ordering / lost-wakeup deadlock** interacting with SEQPACKET
send backpressure on the server's async writer — the guest waits on fd A while its unblocking datum sits on
fd B (12B unread) and the server's reply for yet another op is stuck in Send-Q to fd C. The victim varies per
hang (libtool `sh`, configure `ld`, launchd) because it's a timing race in the multiplex, not one fixed op.
This is deeper than the ring reply-channel choice — it lives in the guest RPC socket-demux + the server
async-writer/SEQPACKET-backpressure interaction, and is very likely present (rarer) even without the ring
(the ring just widens the race window, matching RING_ON hangs a lot / RING_OFF baseline clean-but-not-proven-
immune). A correct fix needs to target that demux/wakeup + writer-backpressure path with per-fd tracing, and
is a multi-iteration darlingserver-internals effort — NOT a ring-wait one-liner. PAUSED here: root cause
localized to the socket layer, prod byte-identical + doctor GREEN. Interim: `DARLING_SERVER_FAST_OPS=0` (ring
inline off) is the lowest-hang-rate proven posture.

## UPDATE 5 — PER-FD DEMUX capture: it is NOT the ring; it is a DGRAM-RPC-wait vs SEQPACKET-aux-channel split
Per-fd capture (baseline binaries; `/proc/<tid>/syscall` gives the EXACT fd each parked thread waits on,
cross-referenced with `ss -x` queues; evidence tmp/a0_fd_snapshot.HANG.txt). At a reproduced hang the wedged
process is the guest **launchd** (guest pid 1), and the picture is precise:
  - launchd's 3 threads are each blocked in `recvmsg` (syscall nr 47) on their **DGRAM RPC sockets in the
    perf#21b high fd-band**: fds 8191 / 8190 / 8189 (band = [TOP-BAND, TOP) = [7680, 8192)). Nothing wrong
    with those waits — that's the normal per-thread RPC reply-wait.
  - Meanwhile TWO **SEQPACKET (u_seq)** sockets that launchd owns — fd=14 and fd=39, both LOW (outside the
    band) — each hold **Recv-Q = 12 bytes UNREAD**, and `waited_by_a_thread = NO`: no launchd thread is
    reading them. Their server peers (darlingserver fd=16 and fd=91) each show **Send-Q = 768** stuck.
So the guest RPC path (DGRAM sockets, mldr.c __mldr_create_rpc_socket = SOCK_DGRAM + sendto to the server
address, band-relocated) is DISTINCT from the stranded channel: the stuck 12-byte messages sit on a
**SEQPACKET connected auxiliary channel** (guest<->server, low fds 14/39 <-> server 16/91) that NO thread is
draining because every thread is parked on its DGRAM RPC reply. The 768-byte server Send-Q is the server
trying to push MORE onto that same undrained SEQPACKET connection -> SEQPACKET backpressure, never flushed.
=> The bug is the **interaction between the DGRAM RPC reply-wait and an undrained SEQPACKET aux channel**
(fork-checkin / S2C / kqchan family — the connected per-client channel, dar-gwn.6.x territory), NOT the ring
reply-channel choice and NOT perf#21b's relocation per se (the band fds behave correctly; the stranded fds
are the LOW un-relocated SEQPACKET ones). This also explains why BOTH earlier fix attempts failed: they
targeted the ring DGRAM reply path, which is not where the 12 bytes strand.
NEXT (one build cycle, approved): instrument the SERVER per-connection — log, for every message sent on a
guest's SEQPACKET aux channel, `(target guest nsid, channel kind, callnum/s2c-number, bytes, send result)`,
and for every such inbound message the same — reproduce once, and read WHICH 12-byte SEQPACKET message type
strands on fd 14/39 and which thread was supposed to drain it. That names the exact aux-channel op and pins
the fix (drain-ordering / wakeup on the SEQPACKET channel vs the DGRAM RPC wait). Prod byte-identical
(dylib 6bd251c3, srv 835946f9, mldr/dyld baseline), doctor GREEN, guest torn down.

## UPDATE 6 — EXACT OP NAMED + FIXED: kqchan proc-notification lost-wakeup deadlock (A0 RESOLVED)
The server SEQPACKET instrumentation (env-gated `DARLING_SERVER_AUXLOG=1`, kqchan.cpp, commit 18349e6;
log at `<prefix>/private/var/log/dserver-auxlog.txt`) named the exact op on the first captured hang
(tmp/a0_auxlog_HANG.txt + a0_auxlog_snapshot.HANG.txt). It is the **kqchan `EVFILT_PROC` notification**
channel — NOT the perf#18 ring, confirming UPDATE 5. Precise picture:
  - The ONLY SEQPACKET socketpair guest<->server is the kqchan channel (kqchan.cpp:48). The stranded
    12-byte payload is exactly `dserver_kqchan_call_notification_t` (callhdr = enum4+int4+int4 = 12B).
  - Wedged channel: `id=259`, server `socket_fd=92`, `listener_nsid=1` (launchd), `target_nspid=415` — a
    build shell that launchd tracks with NOTE_TRACK. Under `brew`'s fork storm pid 415 forks children
    (2699,2700,...) nonstop; every NOTE_FORK pushes an event onto the channel's server-side `_events`
    queue. Trace shows the queue climbing 24->36, EVERY `_sendNotification` `GATED` (`canSendNotif=0`),
    the last `proc._read ACK` (#777) at t=…728.289, then a 27s dead gap = frozen. Guest fd 39 holds one
    stale 12B notification; server fd 92 Send-Q=768 backed up.
ROOT CAUSE (lost-wakeup deadlock): the guest acks a notification by reading it (libkqueue
`evfilt_proc_copyout` -> `proc_read`), which runs server `Kqchan::Process::_read()`:
`canSendNotification=true; deferNotification=true;` (defer so a fresh notification can't overtake the
read reply and desync the in-order SEQPACKET channel), send reply, `_sendDeferredNotification()`.
The defect: `_sendNotification`, when deferred, CONSUMED `_canSendNotification` **and** stashed the ping
into `_deferredNotification`, while `_sendDeferredNotification` only re-sent an *already-stashed* Message.
But the event side (`_notify`, under `_mutex`) and the ack side (`_read`) serialize only on
`_notificationMutex`, NOT in program order across the split `_mutex`/`_notificationMutex`. Under the storm
a `_notify` could stash its ping AFTER the `_read` that would have flushed it already ran -> the stash was
orphaned, `_canSendNotification` stayed false, every later `_notify` GATED forever -> guest never
re-notified -> never sends another `proc_read` -> `_sendDeferredNotification` never runs again =
permanent deadlock. (Ring-OFF narrows but doesn't cure it — the kqchan channel is independent of the
ring, which is why the perf#18-targeted UPDATE-3/4 attempts failed.)
FIX (commit c7ea6d9, kqchan.cpp + kqchan.hpp): make notification delivery LEVEL-TRIGGERED + self-healing.
  - `_sendNotification` when deferred no longer consumes the gate or stashes a Message; it sets a sticky
    `_notificationRequestedWhileDeferred` flag.
  - `_sendDeferredNotification`, on clearing deferral, re-drives `_sendNotification` if a notification was
    owed OR the channel still `_hasPendingEvents()` (new virtual; `Process` overrides -> non-empty
    `_events`). The ping is contentless + duplicate-safe by design (guest reads, finds nothing, drops via
    0xdead), so a spurious re-arm is harmless but a lost one can no longer deadlock. Lock order preserved
    (`_mutex` before `_notificationMutex`, never nested-reversed; `_notificationMutex` dropped before
    `_hasPendingEvents`/`_sendNotification`). Legacy-stash drain kept defensively. Symmetric (fix is in the
    shared base class, so mach-port channels benefit too).
VALIDATION: instrumented server built from ~/work/darling-build. Boot smoke GREEN (server + launchd +
children up; auxlog PUSH/ACK balanced). Brew ring-ON A/B: **the fix is CORRECT BUT INSUFFICIENT — brew
STILL HANGS with it (1/1 on the first repro attempt).** The after-fix hang evidence
(tmp/a0_auxlog_HANG_afterfix.txt + a0_auxlog_snapshot.HANG_afterfix.txt) REDIRECTS the root cause and is
the important finding:
  - The fix works as designed: the re-arm fires (26 REARM lines) and after `id=269`'s last ack (#573,
    t=…433.881) the server DID `sendNotif PUSH` notification #573 to the outbox (t=…433.888) —
    `canSendNotification` correctly toggled. The notification is genuinely on the wire (guest fd 39
    Recv-Q = 12 bytes, delivered).
  - But the guest NEVER reads it: no proc_read #574 ever arrives, `id=269` sits at canSendNotif=0 with
    `_events` climbing to 240, frozen ~24s. The 768B server Send-Q is the server (correctly) continuing to
    try to push while the guest doesn't drain.
=> The kqchan notification backlog is a **downstream symptom, not the cause**. The true stall is that the
guest **launchd never returns to its kevent()/epoll_wait loop to consume the notification**, because a
launchd thread is wedged elsewhere — parked in a DGRAM RPC `recvmsg` (the original per-fd finding, UPDATE
5: fds 8189-8191) whose reply never arrives. `clients_blocked_in_rpc` is `workqueue.depth+threadsBusy`
(server-side work in flight) — it says NOTHING about a guest waiting for a reply already sent or dropped,
so its "0" is consistent with a lost/stranded RPC reply. This is back in the lost-RPC-reply-under-load
family (dar-gwn), but now DOUBLY narrowed: NOT the perf#18 ring (ring-OFF was clean; UPDATE 2/5) and NOT
the kqchan notification gate (this fix delivers notifications reliably and brew still hangs). The next
layer is GUEST-SIDE: identify WHICH RPC call launchd's parked thread awaits on fd 8189-8191 and why its
reply never lands (server dropped it / published to a channel the guest isn't reading / signal-interrupt
race), while its kevent loop — the only thing that would drain the kqchan and unblock the tracked build
shell — is starved behind that same stuck RPC.
STATUS: the kqchan lost-wakeup fix (commit c7ea6d9) is a genuine correctness improvement (a real
defer/gate race that CAN orphan a notification) and is kept on branch fix/a0-kqchan-auxlog, but it is NOT
the brew-hang cure. Prod RESTORED byte-identical to baseline srv 835946f9 (fix NOT deployed). AUXLOG
instrumentation retained behind the env gate for the guest-side investigation. NEXT: guest-thread-stack +
per-fd + server-RPC correlated capture to name the stuck RPC call and its missing reply.
