# A0-ARCH stage 3: interrupt as cancellation (kills #114 stack borrowing)

Status: **IMPLEMENTED + fixes 1/3/4 (fix 2 reverted), 2026-07-04. Binary d81b5cf1
deployed (intentional drift; baseline still 7692d9f6 until landing).
cvstorm2-throttled repro: 6/6 GREEN with the storm at full rate (~14.8k hits/20s)
-- the #114 acceptance grain is solved. REMAINING: the gate ladder (see HANDOFF
at the bottom).** Branch `fix/a0-arch-redesign`.
Spec: tests/A0-ARCH-REDESIGN-SPEC.md stage 3. Prior: tests/A0-ARCH-STAGE2.md.
Acceptance: `A0_STRICT=1 tests/a0-repro/a0-gate.sh full` GREEN (cvstorm2 throttled AND
flood legs stop crashing) = the #114 close-out criterion.

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
ccbe517 (fix 2, REVERTED by 9cbde1e), 5020b3f (fix 3), fix 4 (atomic consume).

- **fix 1** (2d2cd37): first synth gate wedged nestwait -- a nested interrupt_enter
  dispatching ONTO the Parked restored context consumed the cancellation wake as its
  resume permit, then took the stacking branch; the nested sigexc_enter finds the
  wait already finalized and mints no replacement. Gated the consume on
  `!_pendingCall`. Gate went 15/15... but see fix 4.
- **fix 2** (ccbe517) was WRONG and is REVERTED (9cbde1e): psynch mtx/rw
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
