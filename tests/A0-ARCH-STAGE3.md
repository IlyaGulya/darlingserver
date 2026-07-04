# A0-ARCH stage 3: interrupt as cancellation (kills #114 stack borrowing)

Status: DESIGN (2026-07-04, stage 2 complete at 7692d9f6). Branch `fix/a0-arch-redesign`.
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
