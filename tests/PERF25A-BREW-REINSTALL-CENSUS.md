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

## UPDATE 7 — guest-RPC capture: the true stall is launchd wedged in a DGRAM RPC (Recv-Q=0, idle server)
Ran the guest-side capture (tmp/a0_guest_rpc_capture.sh + loop) on BASELINE binaries (srv 835946f9, ring
compiled ON) — the pure prod hang, no instrumentation. Caught the hang; evidence
tmp/a0_guest_rpc_snapshot.HANG_dualns.txt. KEY METHOD FIX: the guest container has its OWN network
namespace, so a host `ss -x` does NOT see the guest UDS sockets — must `nsenter -t <mldr-pid> -n ss -x`.
Decisive facts at a reproduced hang (victim = a CommandLineTools clang, 4 worker threads, + the launchd/
memberd/securityd/shellspawn daemons):
  - SERVER IS FROZEN: rpcs_serviced / ring_serviced_spin / ring_fast_hit are IDENTICAL across two snapshots
    3-4s apart (476535 / 20964 / 14518, unchanged). No workqueue depth, no busy workers. The server is idle
    — it does NOT think it owes anyone a reply.
  - Every parked guest thread (launchd ×3, memberd, securityd, clang ×4) sits in `recvmsg` (nr 47) on its
    **DGRAM RPC band fd** (8188-8191). Those fds have **Recv-Q = 0** (they do NOT appear in the guest-ns
    "nonzero queue" list) — i.e. NO reply is waiting; the guest is genuinely blocked for a reply that was
    never sent.
  - Meanwhile launchd holds **6-7 kqchan SEQPACKET channels** (fds 14/32/34/37/38/39/40) each with
    **Recv-Q = 12** (one undelivered notification), and darlingserver holds the matching 6-7 peers each with
    **Send-Q = 768** backed up. This is the SAME kqchan pile-up as UPDATE 6 — but now confirmed to be purely
    SECONDARY: launchd can't run its kevent()/epoll loop to drain those notifications because ALL its
    threads are blocked in the DGRAM RPC recvmsg above.
CONCLUSION (root class, evidence-locked): the brew hang is a **lost/never-sent DGRAM RPC reply** — a guest
thread (launchd and/or the compiler) is parked in `recvmsg` on its RPC socket with Recv-Q=0 while the
server is idle and believes nothing is pending. The kqchan 12B/768B backlog is a downstream symptom of
launchd being stuck (its kevent loop is the only kqchan drainer). NOT the ring (counters frozen, ring-OFF
clean historically), NOT the kqchan notification gate (fixed; still hangs). This is the dar-gwn
lost-reply family: either (a) the server never received the guest's call (send raced a teardown / wrong
address), or (b) it received it and the reply was consumed by the push_reply / InterruptExit
saved-reply re-delivery path (call.cpp:176-252 / 938-958) and stranded there, or (c) an S2C/signal
interrupt reordered replies and the guest's `PUSH_UNKNOWN_REPLIES` retry (generate-rpc-wrappers.py:1573)
pushed the real reply back and never got it re-sent.
NEXT (needs 1 build cycle): name the exact stuck call. Add a server-side per-guest-tid RPC trace (env-
gated, like AUXLOG): for the launchd/compiler tids, log every call received (number+tid) and every reply
sent (number+target address), so at the hang we see whether the server ever received the parked thread's
last call and whether it sent/dropped/mis-routed the reply. That names the call and the exact drop site.
Prod byte-identical baseline 835946f9, doctor GREEN, guest torn down. Evidence + harnesses in job tmp.

## UPDATE 8 — per-tid RPC trace BUILT + RUN: it is NOT a lost RPC reply and NOT a missed listener edge. UPDATE 7 was WRONG on two counts.
Built the env-gated per-tid RPC trace (commit darlingserver 8cb1c4e, branch fix/a0-kqchan-auxlog, same
DARLING_SERVER_AUXLOG=1 gate + same dserver-auxlog.txt file so RPCTRACE + kqchan AUXLOG interleave):
RECV at callFromMessage; REPLY-DISP naming the 4 pushCallReply dispositions; SENT-UDS/SENT-RING at the
real send; SENT-FALLBACK at the Call::sendReply funnel; FLUSH-SAVED/FLUSH-DEFERRED at both stash flushes;
PUSHREPLY-* at the push_reply subpaths. Built clean (b600271b), deployed, boot-verified (clean lifecycle
trace, all replies terminal SENT-UDS/RING, zero STASH during boot). Reverted to baseline after.
Harnesses: tmp/a0_rpctrace_{capture,loop}.sh, tmp/a0_launchd_anatomy.sh, tmp/a0_listener_recvq.sh (+loops).
Evidence: tmp/a0_rpctrace_snapshot.txt, tmp/a0_launchd_anatomy.txt, tmp/a0_listener_recvq.txt.

THREE captures at three reproduced hangs (ring ON / FAST_OPS=1, instrumented srv b600271b):

(1) RPCTRACE at hang (tmp/a0_rpctrace_snapshot.txt): for EVERY parked guest thread (launchd x3, memberd,
    securityd, shellspawn, a compiler leaf), the thread's LAST server event is a COMPLETED
    `REPLY-DISP ... disp=SEND` immediately followed by `SENT-UDS`. There is NO RECV-without-reply, NO
    STASH-without-FLUSH anywhere (whole run: 36 total stash/flush/fallback events, all resolved; the 16
    SENT-FALLBACK are call=2/code=0 checkin-class, benign). So the server did NOT drop or strand a reply.

(2) SERVER NOT FROZEN (corrects UPDATE 7's "server idle/frozen"): across two snapshots 3-4s apart
    replies_sent / messages_received ADVANCE (e.g. 473161->473163, 476637->476639). memberd runs a 5s
    `semaphore_timedwait` (call 62) + `mach_msg_overwrite` (call 38) poll loop that the server services
    every 5s. UPDATE 7 read "frozen" off too short a window against memberd's slow cadence — an artifact.

(3) LISTENER Recv-Q = 0 (falsifies the missed-edge hypothesis): the server's DGRAM listener
    (.darlingserver.sock, fd 3) has Recv-Q=0 at the hang (ss -x AND /proc/net/unix rx_queue=00000000).
    The listener is registered EPOLLET (server.cpp:562), so a missed edge WOULD leave the guest's request
    unread in this Recv-Q — it is empty. The guest's request is NOT sitting unread on the server.
    (receiveMany drains to EAGAIN correctly; not the leak.)

(4) launchd anatomy (tmp/a0_launchd_anatomy.txt): ALL 3 launchd threads blocked in `recvmsg` (nr 47) on
    the DGRAM RPC band fds 8191/8190/8189, kernel stack __unix_dgram_recvmsg -> __skb_wait_for_more_packets,
    each Recv-Q=0 (empty "fds with Recv-Q>0" list). launchd has ONLY these 3 threads — no separate kevent
    thread visible; whichever thread runs the kqueue loop is one of these 3, all parked in RPC recvmsg.

(5) kqchan SEQPACKET backlog PERSISTS but VARIES: 2 channels at Send-Q=768 this hang (fd 93, fd 18), 6-7 in
    the baseline UPDATE-7 hang. = server pushed 64 x 12B EVFILT_PROC notifications the guest never drained
    (guest kevent loop can't run — its thread is stuck in RPC recvmsg). Still downstream, still varies.

(6) INTERMITTENT ~50%: under the instrumented binary, brew reinstall xz COMPLETED cleanly once (🍺 built in
    1 minute, full configure+make+make-check+install) then hung on the next attempt. So this is a TIMING
    RACE, not a deterministic deadlock — consistent with a lost-wakeup, not a structural cycle.

NEW ROOT CLASS (evidence-locked, replacing UPDATE 7): NOT the ring (counters advance, ring-OFF historically
clean), NOT the kqchan notification gate (fixed c7ea6d9; still hangs), NOT a lost/dropped RPC reply (every
stuck thread's reply was SENT-UDS; no stash stranded), NOT a missed listener edge (listener Recv-Q=0). The
guest thread's LAST RPC completed and it is parked in recvmsg for a reply to its NEXT request — but the
server has NO RECV for that next request AND the request is NOT in the listener Recv-Q. Two survivors:
  (B') GUEST-SIDE lost reply-wakeup: server SENT-UDS the reply, it left the server, but the guest's own
       recvmsg never woke (reply datagram lost/unconsumed on the GUEST socket). Need the guest DGRAM-band
       Recv-Q at the hang — the dual-ns ss lookup keeps returning '?' for fds 8189-8191 (inode not
       resolving in the guest netns dump), the ONE gap blocking a clean B' verdict.
  (C') kqchan-drain circular wait: the launchd thread that must recv() the SEQPACKET notification (to run
       the kevent loop) is itself blocked in an RPC recvmsg whose reply depends on the guest making
       progress that needs the kqueue drained. The Send-Q=768 backlog every hang keeps C' alive.
NEXT (no build; pure capture): resolve the guest DGRAM-band Recv-Q at the hang (fix the inode->ss mapping,
e.g. read the guest socket's rx_queue directly from the guest-netns /proc/net/unix by matching the fd's
inode, since `ss -x` mis-resolves autobind abstract names). Recv-Q>0 on a parked DGRAM fd => B' (guest
lost-wakeup, fix on the guest recv side). Recv-Q==0 everywhere + Send-Q=768 kqchan => C' (kqchan-drain
circular, fix by making the kqchan notification/read not depend on a thread that can be RPC-blocked, or by
a server-side timeout/kick). Prod byte-identical baseline 835946f9, doctor GREEN, guest torn down.

## UPDATE 9 — B' FALSIFIED (guest Recv-Q=0) AND the ring is EXONERATED: ring-OFF hangs 4/6 on the PURE baseline binary. The memory's "RING_OFF clean / A0 = ring split-brain" is WRONG.
Fixed the guest-side Recv-Q lookup (read guest-netns /proc/net/unix rx_queue by inode from /proc/<tid>/fd,
NOT `ss -x` which mis-resolved the autobind abstract names) and caught a hang (tmp/a0_guest_recvq_procnet.txt,
harness tmp/a0_guest_recvq_procnet.sh + loop). Then ran the ring A/B on the PURE baseline binary
(tmp/a0_baseline_ab.sh). Results:

(1) B' FALSIFIED: at the hang, EVERY parked guest DGRAM RPC socket (fds 8189/8191) has GUEST-side Recv-Q=0
    — confirmed BOTH via ss -x (now correct) AND /proc/net/unix rx_queue=00000000. The reply is NOT sitting
    unread on the guest. No guest-side lost-wakeup on the DGRAM band. (The only Recv-Q>0 socket in the whole
    guest ns was /dev/log at 108B — journald, unrelated.)

(2) REPLY ACCOUNTING IS CLEAN: per-tid RPCTRACE at the hang shows every reply accounted for and SENT —
    e.g. launchd tid nstid=1: 1349 REPLY-DISP = 1314 SENT-UDS + 35 SENT-RING, 0 stranded; nstid=23: 14090
    SEND = 14085 UDS + 5 RING + 1 STASH-SAVED w/ 3 FLUSH-SAVED (interrupt path, resolved). Each frozen
    thread's ABSOLUTE last event is a completed SENT-UDS. So the server did not drop/strand any reply.

(3) THE RING IS EXONERATED (the big correction): DARLING_SERVER_FAST_OPS=0 (ring fully OFF) on the
    INSTRUMENTED binary hung 4/6; on the PURE BASELINE binary 835946f9 (no AUXLOG, prod byte-for-byte) it
    ALSO hung 4/6 (2 clean 51-52s, 4 WATCHDOG). ring-ON hangs at a similar ~33-50%. => the hang is
    TRANSPORT-INDEPENDENT and reproduces with the ring disabled. The memory bead (perf#25a #113 correction:
    "A/B on deployed baseline = RING_OFF 2/2 CLEAN 🍺54s", root cause = ring<->UDS split-brain perf#8) was
    based on a 2-SAMPLE ring-OFF run that got lucky. At n=6 ring-OFF is NOT clean. The ring split-brain
    (gr_wait_reply guard-exhaust vs _publishReplyToRingLocked) is NOT the brew hang. DO NOT pursue the
    "symmetric ring/UDS reply" fix for A0 — it targets a non-cause.

(4) INSTRUMENTATION IS NOT A CONFOUND: baseline (no trace) and instrumented (AUXLOG write on every reply)
    hang at the SAME 4/6 rate. The trace is a faithful observer; its added latency does not induce the hang.

WHAT SURVIVES (transport-independent, shared by ring-ON and ring-OFF): the hang lives in machinery common to
BOTH paths — candidates now (a) the single-threaded server event loop (server.cpp:739 receiveMany + inline
doWork, EPOLLET listener), (b) the signal-interrupt protocol (interrupt_enter/exit + push_reply saved-reply)
which fires under brew make-check's SIGCHLD/fork storm regardless of transport, (c) a guest-side psynch /
semaphore_timedwait wait (the frozen threads' last calls are 31=pthread_canceled, 38=mach_msg_overwrite,
62=semaphore_timedwait — the psynch/signal path). The cascade is broad: one hang showed 16 threads across
many pids all parked in __skb_wait_for_more_packets = everything waiting behind one stuck party (a daemon).
NEXT: since it is NOT the ring and NOT a dropped reply, the highest-value probe is to catch the FIRST thread
to freeze (not the cascade) and its exact last call + whether that call got a RECV — i.e. add a monotonic
"freeze order" to the capture (poll wchan every 1s, record the first tid to enter __skb_wait and STAY), then
read its full RPCTRACE tail to see if its last request got a RECV+reply or vanished. Prod byte-identical
baseline 835946f9, doctor GREEN, guest torn down. Evidence: tmp/a0_guest_recvq_procnet.txt,
tmp/a0_baseline_ab.sh output (baseline ring-OFF 4/6 HUNG).

## UPDATE 10 — ROOT CAUSE PINNED TO ONE DROP SITE: a `psynch_mutexwait` reply stranded in `_pendingSavedReply` by an interrupt_exit/push_reply/interrupt_enter ordering race

The first-to-freeze capture (documented next step from UPDATE 9) plus SUDO kernel stacks + full RPCTRACE
tail nailed A0 to **one stranded reply**, not a broad/unknown cause. This SUPERSEDES both the ring
hypothesis (UPDATE 9, still correct that it's exonerated) AND the fork-wait-semaphore hypothesis (the
`dar-gwn.6.5` sticky flag is a real but DIFFERENT bug — it is present in the deployed baseline 835946f9 and
the hang still reproduces WITH it, exactly as that PR draft's own "Note" predicted a deeper residual).

HARNESSES (job tmp, instrumented binary b600271b @ branch fix/a0-kqchan-auxlog HEAD 4e04fe4, ring ON):
  - a0_firstfreeze2.sh: poll wchan 1s; declare a DURABLE freezer only after >=45 continuous polls in
    __skb_wait (v1 a0_firstfreeze.sh fired at poll 3 on a TRANSIENT work-burst wait — too eager; a real A0
    hang is permanent). Records per-tid entry poll + streak, snapshots originator, leaves auxlog intact.
  - a0_sudostacks.sh: same, but SUDO-reads /proc/<tid>/{syscall,stack,fd} for every parked mldr thread
    (non-root reads came back EMPTY = needs CAP_SYS_PTRACE on the guest procs). This gave the kernel stacks.

KERNEL STACKS AT A DURABLE 40s FREEZE (a0_sudostacks_1629201.txt): the ~8-thread cascade splits two ways:
  - MOST parked threads: recvmsg on the guest RPC socket (fd 8189/8191) via __unix_dgram_recvmsg ->
    __skb_wait_for_more_packets. Waiting for a server RPC reply.
  - one thread in accept()/fd3 (unix_accept -> __sys_accept4): a fork parent whose last call was
    fork_wait_for_child (call=11), now waiting to accept a child that never connects back.
  - one thread (nstid 370) whose last call was psynch_mutexwait (call=73) — blocked acquiring a pthread
    mutex a now-stuck sibling holds.

THE DECISIVE TRACE (nstid 370's final 660us, from the auxlog):
  1629215.095805  call=14 (interrupt_enter)   disp=SEND   <- an interrupt episode ENTERS
  1629215.096061  call=15 (interrupt_exit)    disp=SEND   <- ...and EXITS/tears down 256us later
  1629215.096339  call=73 (psynch_mutexwait)  disp=SEND   <- the interrupted call's reply computed as SEND
  1629215.096464  PUSHREPLY-STASH slot=pendingSaved       <- but push_reply STASHES it into _pendingSavedReply
  (no PENDINGSAVED->INTERRUPTTOP, no FLUSH-SAVED ever follows -> stranded forever)

GLOBAL ACCOUNTING PROVING IT IS THIS ONE SLOT (whole run, 1.8M/12.7M trace lines):
  PUSHREPLY-STASH pendingSaved = 1   (nstid 370, the mutexwait, at 1629215.096464 — its LAST line)
  PENDINGSAVED->INTERRUPTTOP   = 0   (the promotion that would rescue it NEVER RAN)
  PUSHREPLY-STASH interruptTop = 4 } all matched
  REPLY-DISP disp=STASH-SAVED  = 5 } by
  FLUSH-SAVED                  = 9 } FLUSH-SAVED (>= stashes) — every OTHER interrupt-stash flushed cleanly.
Exactly one reply in the run is stranded, and it is the psynch_mutexwait for the thread the whole tree
cascades behind.

MECHANISM (the residual bug, call.cpp:296-330 + thread.cpp:2516-2532):
  When a call is interrupted by a signal after the server already sent a provisional reply, the guest bounces
  that reply back via push_reply. The handler at call.cpp:298 checks `_pendingCall->number()==InterruptEnter`;
  if a (new) interrupt_enter is pending it PARKS the reply in `_pendingSavedReply` (line 302), trusting a
  future `_handleInterruptEnterForCurrentThread` (thread.cpp:2522) to PROMOTE it onto `_interrupts.top().savedReply`
  (-> flushed at interrupt_exit). Under brew make-check's SIGCHLD/fork storm the three events
  interrupt_exit + push_reply + next interrupt_enter interleave such that the pending-enter that was detected
  is torn down (or returns via a path) WITHOUT running the promotion — so `_pendingSavedReply` is never
  promoted, never flushed, never sent. The guest's recvmsg for that reply blocks forever; the mutex it was
  acquiring is never released; every peer needing that mutex/that process's progress piles into __skb_wait.
  Intermittent (~33-67%) because it needs the exact 3-way ordering coincidence; transport-independent because
  it is entirely in the signal-interrupt/reply-stash layer ABOVE ring/UDS (hence ring-OFF hangs identically).

WHY THE STICKY-FLAG FIX DOESN'T COVER IT: dar-gwn.6.5 fixes a LOST fork-wait SEMAPHORE edge (parent starves
in waitForChildAfterFork). This hang is a LOST REPLY in the `_pendingSavedReply` interrupt slot for an
arbitrary interrupted call (here psynch_mutexwait) — a different object, a different code path. Both are
children of the same disease: a signal-forced wait/interrupt teardown racing a wakeup/reply handoff.

STATUS: root cause NAMED and observable. NO fix written yet (the interrupt/reply-stash path is boot-critical
duct-tape; a fix must be surgical + gated on BOTH boot smoke AND brew A/B in BOTH ring modes). Candidate
directions (not yet chosen): (a) at push_reply, if a matching interrupt_enter promotion cannot be guaranteed,
send the reply directly instead of parking (mirror the `_interrupts.empty()` direct-resend branch at
call.cpp:305-322); (b) on interrupt_exit teardown, drain any orphaned `_pendingSavedReply`; (c) make the
enter-detected park + promotion atomic under `_rwlock` so an exit can't slip between them. Prod restored
byte-identical to baseline 835946f9 after capture; doctor GREEN. Evidence: tmp/a0_firstfreeze2_1628865.txt,
tmp/a0_sudostacks_1629201.txt, auxlog nstid=370 tail.

## UPDATE 11 — CORRECTION: UPDATE 10's stranded-reply is NOT the reproducible cause. 3/3 fresh hangs = guest-side LIVELOCK with a HEALTHY, SATURATED server (balanced reply accounting)

A confirmation loop (tmp/a0_confirm_dropsite.sh: catch N independent durable >=40s freezes, print each one's
reply-stash accounting) FALSIFIED UPDATE 10 as the general root cause — the same over-claim pattern as
UPDATE 7 (a single capture's apparent smoking gun that didn't generalize). Result across 3/3 independent
freezes (ring ON, instrumented b600271b):
  attempt 1: pendingSaved-stash=0 interruptTop-stash=5 promotions=0 flushes=16
  attempt 2: pendingSaved-stash=0 interruptTop-stash=4 promotions=0 flushes=12
  attempt 3: pendingSaved-stash=0 interruptTop-stash=2 promotions=0 flushes=3
In NONE of the 3 is a reply stranded: pendingSaved-stash=0 (the UPDATE-10 orphan does NOT recur), and
flushes >= interruptTop-stashes so the interrupt-stack stashes all drain. Reply accounting BALANCES.

What the auxlog shows at these freezes instead (attempt-3 tail): the server is NOT idle and NOT stuck — it is
at FULL THROUGHPUT. Final-second event count = 8918; 54 distinct server microthreads active in the last 2000
lines; disposition histogram of the last 3000 events = 985 disp=SEND vs 1 STASH-SAVED; RECV 1017 ~= SENT
(956 UDS + 30 ring). Replies flow normally. Yet the guest workload makes zero forward progress and the
__skb_wait >=40s watchdog fires. The active guest threads are CHURNING, not blocked: nstid 292 (one server
microthread) replays a full process-launch handshake over and over (ring_attach 81, task/host/thread_self_trap
33/34/35, set_thread_handles 8, vchroot_path 3, uidgid 7, mach_msg 38, mach_port_deallocate 39 — the checkin/
bootstrap sequence, repeating); nstid 295 hammers pthread_canceled (call 31) densely (~83ms of back-to-back
issue in the tail). This is a GUEST-SIDE LIVELOCK / non-convergence (a fork/exec/cancel storm that never
settles), NOT a darlingserver stranded-reply deadlock.

REVISED ROOT-CAUSE PICTURE: the "brew hang" caught by the __skb_wait>=Ns watchdog is NOT a single bug. It has
AT LEAST TWO distinct flavors, both of which pile idle threads into __skb_wait behind the stuck/spinning party:
  (A) [1 run, UPDATE 10] a reply stranded in _pendingSavedReply (interrupt_exit/push_reply/interrupt_enter
      race) — REAL for that run but NOT reproducible here; a rare tail case, not the common cause.
  (B) [3/3 runs, this update] guest-side livelock: server + RPC healthy and saturated, guest fork/exec/cancel
      storm never converges. This is the DOMINANT reproducible flavor.
The watchdog (a thread parked in __skb_wait for N seconds) cannot by itself distinguish (A) from (B): in both,
SOME threads block in recvmsg. The discriminator is the reply accounting + server event rate: (A) shows a
stranded stash + a quiescing server; (B) shows balanced replies + a server at full throughput with a churning
guest cluster.

NEXT (no fix; the direction just changed): stop looking for a darlingserver reply-drop. Instrument the GUEST
side of the livelock — which guest userspace loop (libpthread cancellation? posix_spawn/fork retry? a
make/sh/test-harness EINTR loop?) is re-issuing pthread_canceled/relaunch without converging. Candidate probes:
(1) sudo kernel stacks CONCURRENT with a (B) freeze to see the recvmsg-blocked threads' callers vs the
spinning threads' user PCs; (2) a guest-side strace/ltrace-equiv or a DARLING_ counter on pthread_canceled
re-issue; (3) check whether a guest process is stuck in a cancellation point that keeps returning EINTR under
the SIGCHLD storm (ties back to the dtape sigexc clear_wait interaction, but manifesting as spin not stall).
Prod being restored byte-identical to baseline 835946f9; doctor GREEN. Evidence: tmp/a0_confirm_1629686.txt
+ the attempt-3 auxlog accounting above.

## UPDATE 12 — ROOT CAUSE UNIFIED: A0 is a LOST-WAKEUP LIVELOCK in the guest pthread condition-variable (psynch) prepost machinery under the SIGCHLD storm. Flavors A and B are the same disease.

The guest-side livelock capture (tmp/a0_livelock.sh: at a durable freeze, classify each mldr thread SPINNER
[cumulative CPU-ns climbs over a 1s window; kstkeip is 0 in this kernel so spin is detected by schedstat delta]
vs PARKED [flat CPU, in __skb_wait], dump spinners' hammered syscall + kstack, histogram each thread's RPCs)
resolved the mechanism. One multi-threaded guest process (pid ...097) livelocks INTERNALLY:

  SPINNERS (burning CPU, NOT blocked):
    nstid 23  (main)  38M+/6M CPU-ns; re-issues call=71 psynch_cvwait endlessly (SEND every time) +
                       recvmsg-retry spin. It cvwaits, gets woken/EINTR, re-cvwaits — never converges.
    nstid 266         38M CPU-ns; kernel ep_poll (epoll_wait syscall 232, identical args every sample) +
                       hammers call=30 pthread_kill. It spam-signals a peer trying to wake the stuck CV.
  PARKED (idle, 0 CPU, behind the spinners):
    nstid 1,3 (launchd), 6 (memberd), 7, 8 (accept fd3), 188 (psynch_mutexdrop/cvsignal/cvwait), and
    nstid 269 whose last events are cvwait(71)/cvsignal(70)/mutexwait(73) -> PUSHREPLY-STASH slot=pendingSaved
    (the flavor-A orphan — present HERE too, on a parked thread of the SAME livelocked process).

QUANTIFICATION (the lost-wakeup signature): last 5000 events = 1497 pthread_kill(30) + 163 psynch_cvwait(71);
GLOBAL 1145 psynch_cvwait RECV vs only 27 psynch_cvsignal RECV = a 42:1 wait:signal ratio. If the CV protocol
converged, waits and signals would roughly balance; a 42:1 excess of waits means WAKEUPS ARE BEING LOST and
threads re-cvwait forever. pthread_kill fires ~9x per cvwait (nstid 266 trying to nudge the stuck waiter).
Reply accounting still balances at the transport (1 stranded pendingSaved is a co-symptom, not the driver).

THE GUEST SOURCE CONFIRMS THE FAILURE SURFACE (xnu/darling libsystem_kernel emulation
src/.../psynch/psynch_cvwait.c sys_psynch_cvwait): on ret<0 it returns -EINTR; its own comment warns the
negated return drives libpthread `_pthread_psynch_cond_wait`'s "EINTR / prepost recovery", and that mishandling
it "leav[es] an orphaned prepost that permanently strands a later condvar wakeup." Under brew make-check's
SIGCHLD/fork storm psynch_cvwait is EINTR-aborted constantly (every signal clear_wait's the server-side wait —
same dtape_thread_sigexc_enter / clear_wait_internal mechanism the fork-wait sticky flag dar-gwn.6.5 patched);
a rare psynch_cvsignal wakeup races the abort and lands on an already-aborted waiter -> orphaned prepost ->
the CV wakeup is lost -> waiters re-wait forever (1145 vs 27), one peer spam-pthread_kills, one reply strands.

UNIFIED ROOT CAUSE: A0 = lost condvar wakeup in the guest psynch cvwait/cvsignal PREPOST path (and/or the
server psynch cvwait wait that gets clear_wait'd) under signal-forced EINTR aborts during the SIGCHLD storm.
Flavor A (stranded pendingSaved reply, UPDATE 10) and flavor B (spin/livelock, UPDATE 11) are two surfaces of
THIS one bug. It is the SAME family as the fork-wait sticky flag (signal-abort races a wakeup handoff), now in
the pthread CONDITION-VARIABLE machinery instead of the fork-wait semaphore. NOT the ring, NOT a generic
darlingserver reply drop.

FIX LOCUS (no fix written; must be surgical + gated on boot smoke AND brew A/B in BOTH ring modes): the guest
psynch cvwait/cvsignal prepost handling and/or the server-side psynch_cvwait wait's interaction with
clear_wait_internal (dtape sigexc). The prepost-orphan-on-EINTR the guest comment describes is the prime
suspect — verify libpthread's _pthread_psynch_cond_wait recovery actually re-arms the prepost, and that
psynch_cvsignal doesn't drop a signal delivered to a waiter that was just EINTR-aborted. Prod restored
byte-identical baseline 835946f9; doctor GREEN. Evidence: tmp/a0_livelock_1630247.txt + the histograms above.

## UPDATE 13 — FIX FALSIFIED: the timer-cancel-on-sigexc fix does NOT resolve the hang (3/3 HUNG). Root cause still NOT pinned to a line.

The Variant-A fix — cancel the armed wait_timer in dtape_thread_sigexc_enter (thread.c, mirroring the existing
thread_unblock cancel), branch fix/psynch-cvwait-timer-cancel-on-sigexc commit 9c0b96d, binary b714c86f —
was gated: boot smoke + brew reinstall A/B. Result:
  BOOT SMOKE: PASS (fix does not break boot)
  GATE 2 ring-ON: run 1 HUNG, run 2 HUNG, run 3 HUNG  -> aborted the gate; 3/3 is decisive.
The fix does NOT eliminate the hang. => the "stale wait_timer on the signal-abort path fires a spurious
THREAD_TIMED_OUT into the re-issued cvwait" mechanism (UPDATE 12) is FALSIFIED as the driver. The 42:1
cvwait:cvsignal ratio (UPDATE 12) is a real SYMPTOM of a lost condvar wakeup, but the LOSS MECHANISM is not
the stale timer.

HONEST STATUS: three candidate root causes have now been falsified by measurement — the ring (UPDATE 9), the
stranded _pendingSavedReply (UPDATE 11), and the stale wait_timer / timer-cancel fix (this update). What is
SOLID: not the ring, not a transport reply drop, server healthy+saturated, symptom = pthread condvar livelock
(cvwait re-issued forever, cvsignal starved 42:1). What is NOT known: the exact point where the cvsignal
wakeup / prepost handoff is lost under the SIGCHLD/EINTR storm.

METHOD CORRECTION (why the probabilistic brew gate is the wrong tool now): the brew A/B is slow (~1-5 min/run),
noisy, and only tells hang-vs-clean — it can't show WHERE the wakeup is lost, and "0 hangs" is a statistical
claim, not an invariant. The user's requirement is ZERO hangs, period. NEXT = build a DETERMINISTIC
condvar-storm micro-repro: a small guest program with N threads on one pthread condvar (producer/consumer)
while a driver spams signals (pthread_kill) to emulate brew's SIGCHLD EINTR-aborts, aiming to reproduce the
livelock in SECONDS, repeatably, with none of brew. Then instrument cvwait/cvsignal/prepost on THAT to catch
the specific lost wakeup (which generation, who signalled, who waited), fix the exact mechanism, and verify
0/1000 iterations on the repro before any brew A/B. The timer-cancel change is retained on its branch (it is
a correct hardening that matches thread_unblock and passed boot smoke) but is NOT the A0 fix and must not be
described as such. Prod restored byte-identical baseline 835946f9; doctor GREEN.

## UPDATE 14 — DETERMINISTIC REPRO BUILT; it exposes a hard PANIC: "thread already waiting" at waitq.c:2835 (invariant violation on the signal-abort path)

Built cvstorm.c (tmp/cvstorm.c): 8 consumer threads on one pthread condvar + a producer + a "stormer" thread
that spams pthread_kill(SIGUSR1, SA_RESTART OFF) at the consumers, emulating brew make-j's SIGCHLD storm
interrupting psynch_cvwait. Built inside the guest (clang -isysroot .../MacOSX.sdk) and run on the deployed
BASELINE 835946f9 (ring ON). Harness tmp/cvstorm_run.sh (file-based capture, no $(...) wedge).

RESULT — the repro fires in SECONDS, deterministically, and does NOT merely livelock: it PANICS the server.
  BUILD_OK; RUN_START; then:
    darlingserver duct-tape panic: "thread already waiting on 0x...230" @ duct-tape/xnu/osfmk/kern/waitq.c:2835
    sigprocess failed internally while processing Linux signal 10: -111
    semaphore_timedwait failed (internally): -111 ; ./cvstorm: Illegal instruction (core dumped)
This is the SAME subsystem as the brew hang (psynch condvar wait aborted by a signal) but the micro-repro
drives it hard enough to hit the debug invariant instead of only livelocking. Signal 10 = SIGUSR1 (our storm);
in brew the storm signal is SIGCHLD. The -111 (=EINTR mapped) on semaphore_timedwait/signal is the same
interrupt-abort surface. => the repro is a valid, fast, deterministic proxy for the A0 fault site.

MECHANISM (waitq.c:2834 asserts thread->waitq==NULL on entry to waitq_assert_wait64_locked; the panic means a
thread RE-ENTERS a wait while still linked on its previous waitq). Reading the abort path:
  dtape_thread_sigexc_enter (duct-tape/src/thread.c ~503): does
      thread->state &= ~(TH_UNINT | TH_WAIT);   // <-- pre-clears TH_WAIT
      thread->wait_result = THREAD_INTERRUPTED;
      clear_wait_internal(thread, THREAD_INTERRUPTED);
  clear_wait_internal (thread.c:1686): after waitq_pull_thread_locked, at line 1715 checks
      if ((thread->state & (TH_WAIT | TH_TERMINATE)) == TH_WAIT) return thread_go(...);
      else return KERN_NOT_WAITING;
  Because sigexc_enter ALREADY cleared TH_WAIT, this takes the KERN_NOT_WAITING branch and SKIPS thread_go()
  -> thread_unblock() -- the routine that finalizes the unblock (and cancels the timer). The unblock is left
  half-done; the guest's EINTR-retry re-enters assert_wait and trips the thread->waitq!=NULL invariant.
HYPOTHESIS (to be tested on the repro, NOT assumed — two prior root-cause claims were falsified): the
TH_WAIT pre-clear in dtape_thread_sigexc_enter races/short-circuits clear_wait_internal's own state handling.
Candidate fix: let clear_wait_internal do the state transition (do not pre-clear TH_WAIT in sigexc_enter), or
ensure the thread is fully pulled + unblocked before returning. VALIDATION PLAN: the repro is deterministic —
a correct fix must make cvstorm print RESULT=OK (0 panics) across many runs in seconds, THEN brew A/B must be
0 hangs. Evidence: tmp/cvstorm_run_1636387.log. Prod baseline 835946f9, doctor GREEN.

## UPDATE 15 — sigexc TH_WAIT fix VERIFIED to remove the panic, but the lost-wakeup livelock REMAINS (two layers). Repro now reproduces the PURE livelock deterministically in ~1s.

Applied the corrected sigexc fix (commit bd7cdb9: dtape_thread_sigexc_enter clears only TH_UNINT, no longer
pre-clears TH_WAIT, so clear_wait_internal routes through thread_go->thread_unblock and fully pulls the thread
off its waitq). Built binary 50dea03a, ran the deterministic cvstorm repro on it:
  BEFORE (baseline 835946f9): panic "thread already waiting" @ waitq.c:2835 within seconds.
  AFTER  (50dea03a):          NO panic. But: RESULT=HANG — done_count freezes at 97524 after ~1s while
                              storm_hits keeps climbing (14k->101k); consumers stall on cond_wait
                              (per-consumer iters wildly uneven: c7=14 vs c4=44207).
=> TWO LAYERS. The fix removed the waitq-corruption/panic layer (real correctness win). The underlying
LOST-COND-SIGNAL WAKEUP layer is still present — the 42:1 cvwait:cvsignal livelock persists. bd7cdb9 is a
correct partial fix, NOT the complete A0 fix.

GOOD NEWS FOR DIAGNOSIS: with the panic gone, cvstorm now reproduces the PURE livelock deterministically in
~1 SECOND (no brew, no panic masking it) — the clean fast target the investigation needed. Remaining
hypothesis (to be INSTRUMENTED on the repro, not assumed): a psynch cond_signal hands its wakeup off to a
specific waiter that is simultaneously being signal-aborted; the handoff is consumed by the abort and the
signaled work item is stranded (no other waiter picks it up). NEXT: instrument cvwait/cvsignal on the repro
(RPCTRACE auxlog under cvstorm) to catch the specific lost handoff, then fix that layer and re-verify
0 hangs on the repro before brew A/B. Prod restore pending; fix binary 50dea03a still deployed for the next
instrumented repro run.
