# A0-ARCH stages 0–1: fuzzer, typed wake tokens, interrupt-desync survival

Status: IN PROGRESS (2026-07-03). Branch `fix/a0-arch-redesign`. Spec: tests/A0-ARCH-REDESIGN-SPEC.md.
Commits: 2037e24 (stage 0), ef1454f (stage 1), fc69f19 (1b), 27649c8 (-g), 7ae5e05 (1c).

## Stage 0 — scheduling-order fuzzer (DSERVER_SCHED_FUZZ)

Injects the exact event class behind every A0 bug — 2-event dispatch/wake reorders —
deterministically per seed: doWork dispatch defer + spurious extra dispatch, and
wake-while-running forced schedule. Dormant unless `DSERVER_SCHED_FUZZ=<seed>` set
(`DSERVER_SCHED_FUZZ_RATE`, default 1/16 per site). Gate legs `fuzz-sN-*` (A0_FUZZ_SEEDS,
default 2; `A0_FUZZ_GATING=1` to gate — default report-only until stages 2–3 land).

**HEADLINE RESULT: the fuzzer reproduces the disease class ON DEMAND.** Baseline
(a3de8c2 + fuzzer, binary 4b96e6c2): **5 of 6 fuzz legs RED** (server death / guest
SIGABRT) while ALL normal legs stay green. The A0 patches (#113) closed the measured
faces, not the class — the redesign's premise, now measured. Debug cycle for this class
drops from "run brew for hours and hope" to a 15-minute synth gate.

## Stage 1 — typed wake tokens (ef1454f)

`(kind, generation)` wake tokens replace the untyped `_resumePermit` bool. Kinds:
Xnu / Raw / UserSuspension / Kick / Abort; one armed wait + one pending wake per kind
(they nest across interrupt stacking). Waiters ARM before publication (XNU waits in
thread_mark_wait_locked under the thread lock; raw mutex/condvar parks at TAILQ insert,
gen rides in the queue link; sigexc park; kernel-thread birth). Wakers name the wait
(thread_unblock reads+clears `xnu_wait_gen`; mutex/condvar unlock reads the dequeued
link; thread_release/startKernelThread use gen 0 = currently-armed; notifyDead = Abort,
matches anything). suspend()/doWork consume ONLY matching wakes; stale wakes are logged
and dropped. Part 3d subsumed by disarm; Part 3b narrowed to dropping ONLY the pending
Xnu token (a pending Raw wake for the interrupted context's kwq park now survives the
interrupt — the untyped permit machinery lost it). Also closes the old
wait_while_user_suspended FIXME race (arm before re-checking suspend_count).

## What the fuzzer dug up below stage 1 (the finding of the day)

The remaining fuzz reds are NOT token bugs — they are a pre-existing chain, now
reproducible and backtraced (with -g, 27649c8):

1. Dispatch reorder → a guest thread's new call races its still-pending one →
   `setPendingCall` rejection → **-EAGAIN basic reply** (the dar-gwn.6.2 guard).
2. The UDS transport matches replies to requests **by order** (no correlation ids) →
   the EAGAIN + late replies SHIFT the reply stream by one (guest logs
   `BAD RECEIVE MESSAGE: number=25 (expected 81)` cascades; boot procs die with
   `Failed to get started_suspended status: -70`; clang/nestwait abort).
3. The shifted stream makes the guest send `interrupt_exit` out of step → InterruptExit
   **pops `_interrupts` out from under a still-in-flight interrupt_enter fiber** →
   `top()` on empty stack = UB → `jumpToResume(context=0x2)` SIGSEGV
   (thread.cpp:2833, captured 3/3 seeds).

Fixes so far:
- **1b (fc69f19)**: `savedResumeContext` — the interrupted context's ucontext is captured
  at interrupt stacking next to savedStack; jumpToResume restores the SAVED copy, not the
  shared single-slot `_resumeContext` (which the interrupt fiber's own parks overwrite —
  the #114 single-slot clobber shape, now dead).
- **1c (7ae5e05)**: interrupt bookkeeping survives the desync — empty-stack guards at
  every `_interrupts.top()` site + interrupt_exit refuses to pop while the matching
  enter is in flight. Server deaths under fuzz: 3/3 seeds → 1/3 seeds.

## Remaining (owned by later stages)

- The last fuzz server-death shape: `_interrupts` checks in
  _handleInterruptEnterForCurrentThread run UNLOCKED across suspension points; the whole
  jumpToResume/stack-borrowing model is stage 3's target (interrupt-as-cancellation).
- The UDS reply-stream desync (order-based matching + EAGAIN retries) is a transport
  hole: needs correlation ids (like the ring's seq) or an EAGAIN-free busy path —
  guest+server ABI change, scheduled after stage 2. The ring transport is immune (seq'd).
- Guest SIGABRTs under fuzz = the desync reaching guest libc; expected to die with the
  transport fix.

## Gate/perf status

- **Stage-1c synth gate (binary d5daa0ed): ALL 13 GATING LEGS GREEN** — boot, nestwait-off
  4/4, nestwait-on 4/4 (the interim nestwait-on flake is gone), forkwait,
  cvstorm-nostorm 3/3. Known-limit storm legs still red (#114, stage 3). Fuzz survival:
  baseline 5/6 RED → stage-1c **4/6 RED** (fuzz-cvstorm 1/3 red, fuzz-nestwait 3/3 red =
  the unlocked-_interrupts/stack-borrowing shape, stage 3's target).
- **Perf A/B vs 886d13af: NO REGRESSION.** nestwait NO_STORM 3627/3644/3635 jobs/15s
  (mean ~3635 vs fix-baseline ~3679, −1.2%, within historical run spread 3609–3688 and
  under the 3% stop threshold); 300× /usr/bin/true = 2s (identical).
- Full gate (brew xz strict + wget no-freeze): RUNNING — stage-1 landing criterion.
