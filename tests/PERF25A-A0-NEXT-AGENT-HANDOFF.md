# A0 (perf#25a-hang) — handoff for the next agent

Bead: `dar-dar6x4-perf-5dq.15` (perf#8, child-reaping / SIGCHLD+wait4). Task: #113.
Class: `dar-gwn.1.7` / `dar-gwn.6.x` lost-reply family, build-phase.

## One-line status
The `brew reinstall xz` hang is **root-caused to the socket layer** and is **NOT the perf#18 ring**
and **NOT perf#21b fd-band relocation**. It is an **undrained SEQPACKET auxiliary channel**: a guest
thread parks in `recvmsg` on its DGRAM RPC socket while a 12-byte message strands, unread, on a
connected SEQPACKET aux socket the same process owns. Two ring-targeted fix attempts already FAILED
(see "Dead ends"). The approved next step is **server per-connection instrumentation** to name the
stranded aux message + its intended drainer. Do NOT re-attempt a ring reply-wait change.

## The decisive evidence (read these first)
- `tests/PERF25A-BREW-REINSTALL-CENSUS.md` — UPDATE 2/3/4/5 is the full chain. UPDATE 5 is the current truth.
- Raw captures (in the ORIGINAL job tmp `/home/ilyagulya/.claude-work/jobs/ae391f52/tmp/`, may be gone if
  that job was deleted — REPRODUCE if absent): `a0_fd_snapshot.HANG.txt` (per-fd demux), `a0_sock_snapshot.HANG.txt`.
- Capture harnesses (same dir): `a0_fd_capture.sh` (per-fd: `/proc/<tid>/syscall` → fd + `ss -x` queues),
  `a0_sock_capture.sh` (socket Recv-Q/Send-Q), `a0_brew_capture.sh` (brew + 60s-stall watchdog + snapshot),
  `a0_ab_batch.sh` (RING_ON vs RING_OFF rate). All run on BASELINE binaries, no rebuild. Loop them (~1 in 2-3
  runs hangs); the snapshot is written only on a stall.

## What the hang looks like (reproduced, baseline binaries, ring compiled ON)
- Wedged guest = **launchd** (guest pid 1; victim varies per hang — also seen `sh libtool`, configure `ld`).
- launchd's threads each parked in `recvmsg` (syscall nr 47) on their **DGRAM RPC sockets in the perf#21b
  fd-band** (fds 8189/8190/8191; band = `[TOP-BAND, TOP)` = `[7680, 8192)`, mldr.c). Those waits are correct.
- TWO **SEQPACKET (`u_seq`)** sockets launchd owns at LOW fds **14** and **39** each hold **Recv-Q = 12 bytes
  UNREAD**, `waited_by_a_thread = NO`. Server peers (darlingserver fd 16 / fd 91) show **Send-Q = 768** stuck.
- Server otherwise IDLE: `ep_poll`, `workqueue_depth:0`, `workers_busy:0`, `clients_blocked_in_rpc:0`.
=> A 12-byte message (8-byte `dserver_rpc_replyhdr_t` {callnum,code} + 4-byte payload, OR an S2C hdr) is
delivered onto a connected SEQPACKET aux channel that NO guest thread drains, because every thread is blocked
on its DGRAM RPC reply. The 768-byte Send-Q = server pushing MORE onto that undrained connection (backpressure).

## Key architecture facts (verified this session)
- Guest RPC path = **SOCK_DGRAM** (`mldr.c __mldr_create_rpc_socket`: `socket(AF_UNIX, SOCK_DGRAM)` → `dup2`
  into band fd → `sendto` to `__dserver_socket_address_data`). Per-thread, thread-local `t_server_socket`
  (elfcalls/threads.c). Reply target = `thread->setAddress(requestMessage.address())` (server call.cpp:134).
- The stranded sockets are **SOCK_SEQPACKET, connected** — a DIFFERENT channel (fork-checkin / S2C upcall /
  kqchan / lifetime family — the `dar-gwn.6.x` connected per-client channel). Server side uses an AsyncWriter
  (`src/async-writer.cpp`: O_NONBLOCK + Writable-Monitor; buffers on EAGAIN, retries on writable) — this is
  why a non-draining peer leaves 768 bytes stuck in the server Send-Q forever.
- perf#21b fd-band (mldr.c:54+): relocates internal fds to `[7680,8192)`. Band fds behave correctly here; the
  stranded sockets are the LOW un-relocated SEQPACKET ones, so perf#21b is NOT the cause.
- Guest recv hook `dserver_rpc_hooks_receive_message` (dserver-rpc-defs.h) multiplexes: on the DGRAM socket it
  handles inline S2C upcalls (mmap/munmap/mprotect, `call_number==0x52cca11`) then the reply. UDS receive
  loops forever on EINTR (`goto retry_receive` in generate-rpc-wrappers.py) — never abandons.

## APPROVED NEXT STEP — server per-connection instrumentation (one build cycle)
Goal: NAME the stranded aux message and its intended drainer.
1. In darlingserver, add an env-gated (`DARLING_SERVER_AUXLOG=1`) compact log at every SEND and RECV on the
   SEQPACKET aux channel(s): `(target/among guest nsid, channel kind, callnum or s2c-number, byte count,
   send result / EAGAIN-buffered?)`. Prime sites: `Server::sendMessage`/`_outbox` flush, `AsyncWriter::_trySendLocked`,
   the S2C upcall send, the fork-checkin reply, `pushCallReply`. Tag each with the Thread nsid + the socket fd.
2. Build darlingserver ONLY from `~/work/darling-build` (INSTALL_PREFIX=~/work/darling-prefix): `ninja darlingserver`.
3. Deploy to `~/work/darling-prefix/bin/darlingserver` (BACK UP baseline 835946f9 first — see below). dylib untouched.
4. Reproduce via `a0_fd_capture.sh` loop until hang; correlate the log's last aux send to fd 14/39 → read
   which callnum/s2c-number the 12 bytes are and which guest nsid/thread was supposed to drain it.
5. That names the exact aux op. THEN design the fix (drain-ordering / wakeup on the SEQPACKET channel vs the
   DGRAM RPC wait). Every candidate MUST pass BOTH a boot smoke AND the brew ring-ON A/B before deploy.

## Dead ends — do NOT repeat
- Guest `gr_wait_reply` unbounded (`for(;;)` instead of `guard<100000`): **regressed BOOT** (launchd wedges,
  no shellspawn.sock). The UDS fallback is load-bearing. (dylib a6608de1)
- + Server `InterruptExit` saved-reply routed to ring via `_publishReplyToRingLocked`: brew ring-ON still
  hung AND ring-OFF (clean on baseline) ALSO hung → wrong. (dylib 7e311a9b, srv 196801fa)
- Both reverted byte-identical. The bug is not on the DGRAM ring reply path.

## RED/GREEN baseline for validation
- RED: `brew reinstall xz` ring-ON hangs ~1 in 2-3 (RING_ON 3/3 in one batch). RING_OFF (`DARLING_SERVER_FAST_OPS=0`)
  observed clean 2/2 on baseline (NOT proven immune — the aux-channel race just narrows).
- GREEN target: ring-ON completes `🍺 ... built in ~53s` reliably (e.g. 6/6).

## SAFETY (hard rules, verified this session)
- Prod baselines that MUST stay recoverable + restored byte-identical after every measurement:
  dyld `79b22273` (both copies), mldr `f0cd2a82`, **darlingserver `835946f9`**, **libsystem_kernel `6bd251c3`**
  (3 copies: `~/work/darling-prefix/usr/lib/system/`, `~/work/darling-prefix/libexec/darling/usr/lib/system/`,
  `~/work/darling-prefix-homebrew-test/usr/lib/system/`).
- Re-derive baselines if job tmp is gone: darlingserver 835946f9 and libsystem_kernel 6bd251c3 are the
  CURRENTLY-DEPLOYED binaries (confirm via `west darling-doctor` = ALL GREEN before you start). BACK THEM UP
  to your own job tmp before deploying anything.
- Build ONLY from `~/work/darling-build` (Release, INSTALL_PREFIX=~/work/darling-prefix). NEVER the /usr/local
  build tree (`~/work/darling-dev/darling/build`).
- `DARLING_SKIP_DOCTOR=1` inline for intentional-drift build/deploy/boot; run via script files (the build-gate
  hook blocks inline `darling`/`dyld`/`launchd` strings). Hard teardown before each boot.
- Harness gotcha: `darling shell` from a backgrounded subshell fails "Cannot open mnt namespace" — run FOREGROUND
  with a `timeout`. Guest stdout does not always reach a redirected file when the launcher backgrounds; trust the
  brew `🍺` line / on-disk artifacts, not smoke stdout.
- Work on clean `fix/*` branches. Do NOT push. `west dw handoff` before ending. `west darling-doctor` must be
  ALL GREEN at the end (deployed binaries == baseline).

## Where the code is
Workspace root `~/work/darling-dev`. Modules under `~/work/darling-dev/darling/src/external/{darlingserver,xnu}`
and `~/work/darling-dev/darling/src/startup/mldr`. darlingserver currently on branch `perf/shmem-ring-abi-validator`
@ d46b41f; xnu on `perf/shmem-ring-guest` @ caddc2b. Manifest repo `~/work/darling-dev/darling-workspace`.
