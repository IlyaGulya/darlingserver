# A0-ARCH stage 3: interrupt as cancellation (kills #114 stack borrowing)

Status: **LANDED 2026-07-04 (commits 78d7f7b + fixes 2d2cd37/5020b3f/e66aeee,
fix-2 ccbe517 reverted by 1789d62). Binary d81b5cf1 = NEW DOCTOR BASELINE
(deploy-baseline.md5 dserver 7692d9f6 -> d81b5cf1; doctor ALL GREEN). The #114
SEGV stack-borrowing crash class is ELIMINATED (zero interrupt SEGVs across every
stage-3 run); interrupt is now cancellation through the normal dispatch path with
no stack borrowing.** Branch `fix/a0-arch-redesign`. Spec:
tests/A0-ARCH-REDESIGN-SPEC.md stage 3. Prior: tests/A0-ARCH-STAGE2.md.

Landing acceptance (d81b5cf1): synth 16/16, quick gate 19/19 gating green
(nestwait 8/8, brew xz 2/2 strict, both storm known-limit legs OK), ZERO
mstate/xwait violations, ZERO SEGV; cvstorm2-throttled 12/12 OK at full storm.
Two pre-existing non-blockers re-scoped out (proven NOT stage-3 regressions via
A/B vs 7692d9f6): cvstorm2-flood dispatch-starvation HANG -> task #118; rare ~2%
nestwait semaphore_timedwait -111 flake (seen pre-stage-3).

**Stage 4 (single-runner): MEASURED -> NO-GO (2026-07-04).** See the "Stage 4
measurement" section at the bottom. The WorkQueue worker is already de-facto
unused (workers_busy=0 in 19/20 samples under a full-rate nestwait storm;
inline fastpath handles ~100% of dispatch); collapsing it buys no throughput and
removes no real race surface, while touching the hot loop for negative expected
value. #116's goal was met by stage 3. Recommendation: STOP the A0-ARCH ladder
here.

### FLOOD A/B SETTLED (2026-07-04, 2nd session pass) -- landing unblocked

The two #114 grains, measured HONESTLY (not the over-optimistic "6/6" first pass):

- **cvstorm2-throttled** (-DNCONS=1 -DSTORM_THROTTLE_US=1000): **12/12 OK** on
  d81b5cf1. Solid. done ~375M/run. This grain of #114 is closed.
- **cvstorm2-flood** (-DNCONS=1, no throttle = pathological pthread_kill storm):
  a coin-flip HANG (~1/3 of runs), but **NO CRASH** and no server death on EITHER
  binary. A/B (job-tmp s3_flood_ab.sh, robust busy-file swap): 2c baseline
  7692d9f6 = ~11 OK / 5 HANG / 0 crash; stage-3 d81b5cf1 = ~9 OK / 2 HANG / 0
  crash (combined across two A/B rounds). **The flood HANG is PRE-EXISTING on the
  pre-stage-3 baseline -- it is NOT a stage-3 regression.**

**Root cause of the flood HANG (separate, pre-existing bug -- NOT the cancellation
redesign):** dispatch starvation under the interrupt(pthread_kill) flood. The
guest's condvar-signal wakes are not dispatched promptly because the interrupt-RPC
storm monopolizes the main loop; forward progress is gated to the **1 Hz XNU
long-term-timer scan** (`TIMER_LONGTERM_THRESHOLD = 1 sec` on x86_64,
duct-tape/xnu/osfmk/kern/timer_call.c:89). Every guest timeout (30s/5s/4s cond
deadlines) exceeds the 1s threshold, so they sit in the long-term queue and are
serviced only by the periodic ~1s rescan -- captured live in the auxlog as a
perfect 1.000s cadence of non-override TIMER-ARM (job-tmp s3_flood_tt_auxlog +
the arm/skip/fire deltas). So `done` advances ~1 step/sec -> maxstall climbs
3->5 -> RESULT=HANG when a stall crosses STALL_LIMIT=5. This is a fairness/
starvation problem, orthogonal to interrupt-as-cancellation, and belongs to its
own ticket (candidate: a fast-path condvar-wake dispatch that pre-empts the
interrupt storm, or servicing the ready-queue before re-blocking on epoll).

**Landing decision: stage 3 achieved its goal and is SAFE TO LAND.** It eliminated
the #114 SEGV stack-borrowing crash class (zero interrupt SEGVs across every
stage-3 run), the throttled grain is 12/12 solid, and the residual flood
slow-wedge is pre-existing (present identically on 2c). #114's SEGV is closed;
the flood throughput-starvation HANG is spun out as a follow-up.

### Landing gate results on d81b5cf1 (2026-07-04)

- **synth gate** (A0_FUZZ_SEEDS=3 DSERVER_MSTATE_ABORT=1): **PASS=16 FAIL=0 ALL
  GREEN**, ZERO mstate/xwait violations, ZERO SEGV/crash markers. Both #114
  known-limit legs GREEN this run (flood landed on the OK side of its coin-flip).
  Fuzz reds all the pre-existing UDS-desync class ("pre-redesign hole -- not
  gating"); no new failure class, no interrupt crashes.
- **quick/brew gate** (1st run): PASS=16 FAIL=2 -- brew-xz 2/2 GREEN, but two
  gating nestwait legs (nestwait-off-4, nestwait-on-3) RED with
  `semaphore_timedwait failed (internally): -111`. NOT a hang or crash: both legs
  ran the full 15s with healthy progress (jobs climbing, stall=0), then the guest
  aborted at teardown. -111 = -ECONNREFUSED on the semaphore_timedwait RPC (#62);
  the guest emulation (xnu .../mach_traps.c:214) tolerates only -EINTR
  (-> KERN_ABORTED) and __simple_abort()s on anything else.
- **-111 is a RARE PRE-EXISTING FLAKE, not a stage-3 regression** (investigated
  before landing): A/B nestwait 2c-baseline 7692d9f6 vs stage-3 d81b5cf1 (8 iters
  each, fresh boot per leg = matches run_synth) = 0 -111 on BOTH; a 20-iter stage-3
  repro = 0 -111. So across ~44 stage-3 nestwait runs since the single quick-gate
  occurrence, exactly ONE -111 (~2%). Seen historically pre-stage-3 too (PERF25A
  census line 604: same `-111 ; Illegal instruction` signature). Could not capture
  one live to root-cause the ECONNREFUSED-vs-EINTR reply path (rate too low). Fix
  direction when it recurs: capture the failing run's auxlog (RPC #62
  RECV/REPLY-DISP/STASH) -- the cancelled/torn semaphore_timedwait should surface
  -EINTR, not a transport ECONNREFUSED; suspect the guest side treating a
  connection-level error as the RPC return under teardown. Tracked as a low-pri
  known-flake (own note; NOT gating-blocking for stage 3). Landing signal = a
  clean confirmatory quick gate (the ~2% flake clears on a re-run).

Acceptance (original): `A0_STRICT=1 tests/a0-repro/a0-gate.sh full` GREEN. NOTE:
the flood-KNOWNLIMIT leg will still coin-flip RED under A0_STRICT=1 as a HANG (not
a crash) -- that is the pre-existing starvation bug, not a stage-3 failure. Land
on: throttled GREEN + zero-SEGV + no-flood-regression vs 2c, and re-scope the
A0_STRICT flood leg to the follow-up ticket.

## Today's flow (what dies)

interrupt_enter dispatch (doWork) pushes an InterruptContext frame {savedStack,
savedResumeContext, interruptedCall, savedReply} and moves the thread's live slots
aside; the enter call runs on a fresh fiber. _handleInterruptEnterForCurrentThread
then: sigexc_enter (clear_wait -> typed Xnu wake) -> DROPS that wake -> and resumes
the interrupted context SYNCHRONOUSLY via jumpToResume onto the frame's savedStack
(stack borrowing). The resumed call unwinds to syscallReturn, which detours through
setcontext(_syscallReturnHereDuringInterrupt) back onto the enter fiber, ABANDONING
the interrupted frames mid-unwind; the epilogue then frees savedStack from another
frame's bookkeeping. Every #114 face lives here: a psynch continuation that re-takes
the kwq lock and raw-suspends mid-unwind strands/duplicates stack ownership
(SEGV under pthread_kill storms); the EDEADLK self-rwlock face is the same
single-owner violation.

## New flow (cancellation through the NORMAL dispatch path)

1. **Frame push (doWork) unchanged** in shape: the frame still saves
   {savedStack, savedResumeContext, savedContinuation, interruptedCall, savedReply}
   -- two live contexts per thread genuinely exist during the enter window and the
   frame is where the parked one waits. NO behavior change at push.
2. **_handleInterruptEnterForCurrentThread rewrite**:
   - stash _pendingSavedReply into the frame (unchanged);
   - sigexc_enter: clear_wait finalizes the interrupted wait as THREAD_INTERRUPTED
     and fires the typed Xnu wake -- **the wake is NOT dropped anymore**; it is the
     dispatch vehicle for the cancellation unwind;
   - mark the frame {cancellationArmed = true, replyOwed = true}; do NOT reply, do
     NOT jumpToResume; processCall returns normally. The enter fiber completes on
     its OWN stack, which is freed by its OWN doneWorking (single ownership).
3. **Context restore at the enter fiber's doneWorking**: if the top frame has
   cancellationArmed and a saved context, doneWorking (already under _rwlock, after
   freeing the enter fiber's stack) moves the frame's saved context back into the
   thread's normal slots (_stack/_resumeContext/_continuationCallback/_activeCall)
   and transitions **Parked** (repark, reason "repark-interrupt-cancel") instead of
   Idle -- the machine stays honest: the thread HAS a resumable context again. The
   pending deliverable Xnu wake then makes it Ready -> dispatched NORMALLY (doWork
   resume branch, own fiber, own stack). The stranded-wait assertion does not fire:
   this is a resume dispatch, not a fresh one.
4. **The interrupted call unwinds normally**: wait_result == THREAD_INTERRUPTED, the
   continuation/inline wait takes its error path, replies. The reply-stash site
   (pushCallReply) stashes it into the frame keyed on
   `!_interrupts.empty() && call == _interrupts.top().interruptedCall` (a durable
   per-frame fact) instead of the transient _interruptedForSignal window. **At stash
   time, if frame.replyOwed: send interrupt_enter's reply 0 and clear replyOwed** --
   exactly the guest ordering (enter's reply only after the interrupted reply is
   safely stashed), without borrowing anything.
5. **interrupt_exit unchanged** in protocol: pop frame, flush savedReply after the
   exit reply. The 1c desync guards stay.
6. **No-interrupted-call case** (fresh-call interrupt, nothing to cancel): reply 0
   immediately in processCall, frame.cancellationArmed stays false, doneWorking
   idles normally. Continuation case: restore _continuationCallback; the resume
   branch allocates the fresh stack as it always does for continuations.

DELETED: jumpToResume, _syscallReturnHereDuringInterrupt,
_didSyscallReturnDuringInterrupt, _handlingInterruptedCall, _interruptedContinuation,
the syscallReturn/microthreadWorker/microthreadContinuation setcontext detours, the
"interrupt-enter-drop-xnu" wake drop, and savedStack freeing from the epilogue
(stacks die with their fibers, exactly once).

## Watchpoints (from the falsified/known record)

- Part 3g FALSIFIED: do NOT hand-finalize TH_WAIT+waitq==NULL waits. This design
  never hand-finalizes -- sigexc_enter's clear_wait_internal remains the only abort,
  now taped (stage 2c `xwait-sigexc-abort` + `xwait-unblock` w/ THREAD_INTERRUPTED).
- The interrupted call may be mid-S2C (deferReplyForS2C / s2cInterruptExitSemaphore
  usingInterrupt path) -- re-read census UPDATE 24-26 before touching; the S2C
  machinery is NOT part of this redesign.
- Nested interrupts: frames compose as today (each enter saves the then-live slots);
  the restore happens frame-by-frame at each enter fiber's doneWorking.
- A cancelled wait whose handler never replies would leave replyOwed pending
  (guest parked in sigexc) -- same liveness profile as today's flow, minus the crash;
  the tape (InterruptPush without reply-sent Note) makes it diagnosable.
- MicroState legality: doneWorking gains the "repark-interrupt-cancel" edge
  (Running->Parked with a restored context) -- same family as the 2b
  repark-noop-dispatch edge; the fuzzer gates it.

## Plan

1. hpp: InterruptContext {+cancellationArmed, +replyOwed}; delete the five dead
   members; InterruptEnter Call gains deferred-reply support (keep the Call alive
   in the frame until the stash sends its reply).
2. thread.cpp: rewrite _handleInterruptEnterForCurrentThread (order: stash, sigexc
   enter, arm frame; keep the 1c empty-stack guards); doneWorking restore+repark;
   delete the detours; simplify syscallReturn; stash-site rekey + owed-reply send.
3. call.cpp: InterruptEnter::processCall deferred reply; stash path in pushCallReply.
4. Gate ladder: build -> boot smoke -> synth+fuzz (abort-on-violation) -> probe ->
   quick + perf -> then the acceptance run `A0_STRICT=1 full` (storm legs gating).

## Debugging chronicle (2026-07-04, the tape earned its keep 3 more times)

Commits on `fix/a0-arch-redesign`: 78d7f7b (implementation), 2d2cd37 (fix 1),
ccbe517 (fix 2, REVERTED by 1789d62), 5020b3f (fix 3), e66aeee (fix 4, atomic consume).

- **fix 1** (2d2cd37): first synth gate wedged nestwait -- a nested interrupt_enter
  dispatching ONTO the Parked restored context consumed the cancellation wake as its
  resume permit, then took the stacking branch; the nested sigexc_enter finds the
  wait already finalized and mints no replacement. Gated the consume on
  `!_pendingCall`. Gate went 15/15... but see fix 4.
- **fix 2** (ccbe517) was WRONG and is REVERTED (1789d62): psynch mtx/rw
  continuations discarding a committed grant on THREAD_INTERRUPTED is DELIBERATE --
  unlike the cv path, the mutex/rw DROP side compensates a KERN_NOT_WAITING signal
  itself (`_kwq_mark_interruped_wakeup` "interrupt post" / the firstfit redrive in
  `_psynch_mutexdrop_internal`). Honoring the retval waiter-side DOUBLE-GRANTS and
  desyncs the mutex seq protocol. Keep this asymmetry in mind: cv = waiter-side
  carve-out, mutex/rw = signaller-side compensation.
- **fix 3** (5020b3f): the timerfd read handler left the just-fired deadline in
  `_currentTimerDeadline`; every non-override `timer_arm` until dtape_timer_fired's
  re-arm compared against the PAST and was skipped -- a 1ms wait timer armed in the
  window never fired until an unrelated event re-armed the fd (the stormer's usleep
  latching to the 5s ring-duplex fail-closed metronome, storm at ~2/s instead of
  ~750/s). Pre-existing hole; reset the deadline on read. Also added
  TIMER-ARM/-SKIP/-FIRED rpctrace lines (DARLING_SERVER_AUXLOG=1).
- **fix 4**: the REAL form of the fix-1 class. The consume lived in doWork's FIRST
  locked section; the resume-vs-stack branch lives in the SECOND; an interrupt_enter
  landing between the unlocked windows ate the permit and stacked anyway -- the
  restored context reparked forever HOLDING A COMMITTED GRANT (tape verbatim:
  wake-consumed gen 844 -> interrupt-push -> sigexc-abort(NOT_WAITING) ->
  repark-interrupt-cancel -> dispatch-stale -> repark-noop-dispatch; open drained
  frame, armed=[0...]). The consume now happens in the second section immediately
  before the branch: consume iff resume. Section-1 transition reasons are now
  "dispatch-fresh"/"dispatch-onto-parked" (resume fact = the wake-consumed event).

### Debug-harness pitfalls burned into this chronicle (do not repeat)

- `pgrep -x darlingserver | head -1` attaches gdb to a STALE server from the
  previous run; use `--newest` AND verify the walked registry contains the workload
  nstids. Hours were lost reading coherent-looking tapes of leftover container
  daemons (nstid 1/3/6/7 = launchd-era threads, NOT the workload).
- cvstorm2 self-exits at stall>=6 and the gdb attach freezes the server for
  seconds: trigger the walk at stall=2, or the workload teardown destroys the
  interesting threads before the walk reads them.
- The auxlog (dserver-auxlog.txt) is O_APPEND across boots with CLOCK_MONOTONIC
  stamps: rm it BEFORE the run you intend to analyze, or cross-run garbage will
  tell convincing lies.
- The walker (job-tmp thread_state_walker.py) now: uses `_map`, prints the top
  InterruptContext frame {armed/owed/icall/ecall/sstk}, decodes StateEvent::XnuWait,
  dumps 32 tape entries. REG_OFF must be re-checked per binary
  (`nm darlingserver | grep threadRegistryEvE8registry`; 0x187a70 for the whole
  d81b5cf1 lineage). runner_state.py prints kernelAsync queue depth/available.

### Instrument validation + two more corrections (2026-07-04, second pass)

- **Validated the walker against a KNOWN-GOOD boot before trusting it on a wedge**
  (job-tmp s3_walker_selftest.sh): a clean idle boot walks 18 threads, 0 errors,
  real mstate/frames/tapes/armed-pend. Do this first each session -- if the
  instrument can't read a known signal, no wedge walk is trustworthy. All the
  walker's struct reads go through gdb's type system (self-correcting to the live
  layout), so the only per-binary knob is REG_OFF.
- **`dtape=opaque` is PERMANENT, not a regression**: `struct dtape_thread` has zero
  structure_type DWARF in the build (duct-tape TU emits typedefs/functions only).
  So the walker cannot decode xnu.state / wait_result / xnu_wait_gen / mutex_link
  from the C++ `_dtapeThread` pointer -- ever. Do NOT sink time into hand-offset
  math to recover it (variable-layout members = a convincing-lie generator). The
  C++ side (mstate + frames + tape + armed/pend) is sufficient to classify these
  wedges -- the fix-4 wedge was diagnosed entirely from the tape.
- **Tape/auxlog timestamps are MICROSECONDS; do the division.** A 101856-unit gap
  is 0.102s, not 102s. An off-by-1000 misread turns a healthy 100ms idle window
  into a phantom 100s stall. Every "long stall" must be `/1e6` before you believe it.
- **HANG-detection must let the run FINISH.** s3_cap_v2.sh triggers a walk at
  stall=2 and kills the guest -- but in the FLOOD grain stall=2 is TRANSIENT
  (recovers to stall=0 next second; it is slow, not wedged). Walking + killing at
  stall=2 destroys the RESULT line and walks a healthy-but-slow server. To get an
  honest OK/HANG verdict, run WITHOUT the walk (job-tmp s3_flood_verdict.sh, runs
  to completion); only walk when stall actually climbs to STALL_LIMIT (5). The two
  #114 grains are separate gate legs: `cvstorm2-throttled` (-DNCONS=1
  -DSTORM_THROTTLE_US=1000) = **12/12 OK on d81b5cf1, solid**; `cvstorm2-flood`
  (-DNCONS=1, no throttle = pathological storm) = under verdict test. Both are
  run_knownlimit -> non-gating by default, HARD-GATING under A0_STRICT=1 (that is
  the #114 close-out criterion).

## HANDOFF: remaining ladder to land stage 3 (task #116)

Deployed binary d81b5cf1 (= fixes 1+3+4, fix-2 reverted) passed: boot smoke,
echo smoke, cvstorm2-throttled 6/6 GREEN at full storm rate. NOT yet run on it:

1. `A0_FUZZ_SEEDS=3 ./a0-gate.sh synth` -- expect 16/16 gating green; compare fuzz
   survival vs the 4-red baseline (stage 3 should IMPROVE it: the interrupt crashes
   are gone; residual reds should be UDS desync only).
2. Perf A/B (job-tmp s2c_perf.sh pattern): nestwait NO_STORM x3 + 300x true;
   corridor 3609-3692 / 1-2s. Note fix 4 moved the consume but adds no locking.
3. `./a0-gate.sh quick` (brew xz strict) -- landing battery.
4. **Acceptance: `A0_STRICT=1 ./a0-gate.sh full` GREEN** -- storm legs gating.
   That closes #114. If cvstorm2-flood still reds: it is a HANG now, not a SEGV;
   same walker treatment (the crash class is dead -- zero interrupt SEGVs seen
   across every stage-3 run).
5. Land: bump deploy-baseline.md5 to the landing binary, update this doc + memory
   (dar-a0arch-stage1-typed-wake-tokens.md) + task #116, `west dw handoff`, close
   #114 with a pointer here.
6. Then stage 4 measurement (single-runner go/no-go writeup) or stop per spec.

## Stage 4 (single-runner): MEASUREMENT + GO/NO-GO (2026-07-04)

Spec stage 4 (optional, measure-first): collapse the `WorkQueue<Thread>` worker
into the main event loop -> ONE runner, to remove the fiber-migration race
surface (fibers today can run either inline in doWork or on the worker thread,
migrating OS threads via getcontext/setcontext).

**Measurement** (job-tmp s4_measure.sh + statsample.py, on d81b5cf1): sampled the
server stat socket (`darlingserver-stat:<prefix>`, abstract UDS; gauges
workers_total/busy/available + workqueue_depth + inline_handled/rpcs_serviced)
once/sec under (A) a full-rate nestwait storm and (B) a launch-heavy 200x
/usr/bin/true. Stat gauges come from `_workQueue.stats()` (server.cpp:713, the
progress-metrics patch) -- no gdb, no bss offsets.

**Result -- the worker is already de-facto UNUSED:**
- Under the nestwait storm at ~33k RPCs/s (rpcs_serviced 655k->816k over the
  window): **workers_busy = 0 in 19 of 20 samples**; workqueue_depth = 0 except
  two transient depth=2 blips (t=7s, t=14s) that drained by the next sample.
  workers_total=1, workers_available=1 throughout.
- **inline_handled tracks rpcs_serviced ~1:1** (871k vs 816k at t=20s): the perf#2b
  inline fastpath already handles ~100% of dispatch in the main loop.
- Leg B (launch/fork-exec shape): same -- workers_busy=0, depth=0.

**GO/NO-GO: NO-GO (stop).** The premise (bypass/kill the worker to remove
migration races) is moot -- the worker is already bypassed ~100% of the time by
the inline fastpath, so (a) there is no throughput to gain (inline IS the path,
the worker is not a bottleneck being avoided), and (b) fibers essentially never
migrate to the worker in the real dispatch pattern (busy=0 almost always), so the
migration hazard stage 4 targets is already vanishingly rare. Deleting the worker
would touch the hot dispatch loop and force the rare genuine concurrent-ready case
(the depth=2 blips = 2 threads that truly needed to run at once) to serialize --
a small latency/correctness risk for zero measurable benefit. The two-runner split
costs nothing when idle and earns its keep exactly in those rare concurrent cases.

**#116 is DONE:** its goal (kill the #114 SEGV crash class via the wake/wait
protocol redesign) was achieved by stages 0-3 (LANDED, d81b5cf1 baseline). Stage 4
is declined on evidence. Remaining darlingserver hardening lives in separate
tickets: #118 (flood dispatch-starvation), #119 (nestwait -111 flake), plus the
post-stage-2 UDS correlation-ID transport backlog.
