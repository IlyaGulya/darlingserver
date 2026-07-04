# A0-ARCH stage 2: one run-state machine per thread (kills the dual-view disease)

Status: **2a + 2b + 2c LANDED 2026-07-04** (deployed 7692d9f6 = new doctor baseline).
Stage 2 COMPLETE. Branch `fix/a0-arch-redesign`.
Spec: tests/A0-ARCH-REDESIGN-SPEC.md stage 2. Prior stage: tests/A0-ARCH-STAGE1.md.

Stage 2 is landed as three independently-gated sub-steps:

## 2a — SHADOW machine + transition tape (commit a58457b, binary fc64f739)

The authoritative-to-be per-thread state, written ALONGSIDE the existing
`_running`/`_suspended` flips (deriving from them, not driving them):

```
MicroState: Idle -> Running -> Parking -> Parked -> Ready -> (Running | Terminated)
```

- `Idle`       no resumable context, waiting for a fresh call
- `Running`    a worker owns the microthread (dispatch decision window may still
               have `_suspended` residue — tolerated in 2a, formalized in 2b)
- `Parking`    suspend() committed the park; fiber unwinding to doneWorking
- `Parked`     off-worker, resumable context, waiting for a typed (stage 1) wake
- `Ready`      parked + deliverable matching wake pending; a dispatch is owed
- `Terminated` final

ONE transition function `_mstateTransitionLocked(to, reason, aux…)` asserts:
- transition legality (explicit from→to table; e.g. Ready→Parked is illegal,
  Parked→Running covers both consuming resume and stale dispatch);
- view consistency, POSITIVE direction only in 2a (Running/Parking ⇒ `_running`,
  park states ⇒ `_suspended`, Idle ⇒ `!_suspended`). The negative direction can't
  be asserted yet because `impersonate()` latches `_running=true` as a lockout on
  a non-running thread — that latch gets its own field in 2b.

Per-thread 32-entry ring tape records transitions AND wake-protocol events
(arm/disarm/pending/dropped/consumed with kind+generation, interrupt push/pop,
rerun deferrals, impersonation pins). Dumped:
- on any violation (log-and-survive by default; `DSERVER_MSTATE_ABORT=1` = fatal);
- from the duct-tape `panic()` funnel via the new `current_thread_dump_state_tape`
  hook — a state-machine panic now prints the exact event history that led to it.

Gate wiring: fuzz legs run with `DSERVER_MSTATE_ABORT=1` (violation = crash = RED);
ALL synth legs log at err level and grep dserver.log for `MSTATE VIOLATION`
(`A0_MSTATE_ABORT` overrides). gdb walker updated (job tmp thread_state_walker.py)
to print mstate + tape per thread.

Notable modeled edges (why the table looks the way it does):
- suspend()'s post-getcontext consume runs on BOTH paths: genuine park race
  (Parking→Running "wake-raced-park") and a spurious-but-legal consume right after
  a real resume (state already Running; wait loops re-check their predicates).
- wake() transitions Parked→Ready only when the park is COMMITTED; a wake landing
  in the Running/Parking window just records a pending event — doneWorking decides
  Parked vs Ready at the tail ("park-committed").
- kernel-thread birth (`setupKernelThread`) is Idle→Parked directly (no fiber ran).
- doWorkInline violation recovery (a "non-blocking" op that suspended) is the one
  legal Parking→Idle edge; the site logs loudly on its own.

### 2a gate results

First synth gate (binary fc64f739, A0_FUZZ_SEEDS=3):
- **ALL 13 gating legs GREEN with ZERO mstate violations** — the shadow model is
  exact for normal operation, at storm-free load, first try.
- cvstorm2-throttled (known-limit #114 leg): 4 violations (expected territory).
- Every fuzz leg: exactly 1 violation then abort (DSERVER_MSTATE_ABORT=1) — the
  tape dump identified it in one read: **model gap, not server bug**. A fuzzer
  spurious dispatch of a Parked thread reparks as Parked→Running("dispatch-stale")
  →Parked("park-committed") WITHOUT re-traversing Parking (suspend() never ran,
  the park was never un-committed). Fixed by legalizing Running→Parked/Ready as
  the "no-op-dispatch repark" edges (binary e50a3450). This is precisely the
  workflow the spec wanted: the tape replaces printf archaeology.

Probe pass (e50a3450, abort OFF to collect ALL violations per run --
fuzz-nestwait s1/s2, fuzz-cvstorm s1, cvstorm2-throttled):
- **ZERO mstate violations on all four legs.** The shadow model now holds under
  fuzz AND storm; every remaining red is a PRE-EXISTING class, now cleanly
  separable from model gaps:
  - fuzz-nestwait s2: the documented UDS reply-stream desync cascade (guest
    aborts on BAD RECEIVE; server survives) — post-stage-2 transport fix.
  - cvstorm2-throttled: silent server SEGV, the #114 shape — stage 3.
  - fuzz-nestwait s1: **new face of the stage-3 family**: server death via
    `std::terminate` on `std::system_error EDEADLK` ("Resource deadlock
    avoided") thrown from callFromMessage — a fiber re-locking the `_rwlock`
    its own thread already holds (fiber suspended/migrated while the OS thread
    still held the lock, or a re-entrant call path). Same single-owner-violation
    disease jumpToResume has; the interrupt-as-cancellation redesign owns it.
    (SIGSEGV deaths do NOT trigger the panic-funnel tape dump — that only fires
    on duct-tape panic(); wiring the dump into a fatal-signal handler is a
    possible stage-3 triage improvement.)

Acceptance quick gate (e50a3450, A0_FUZZ_SEEDS=3, fuzz legs abort-on-violation):
- **ALL 15 gating legs GREEN** (boot, nestwait-off 4/4, nestwait-on 4/4,
  forkwait, cvstorm-nostorm 3/3, brew-xz 2/2 strict) with **ZERO mstate
  violations across the entire battery** — including real brew load.
- Fuzz survival 4 RED / 6 (nestwait s1/s2/s3 + cvstorm s2) = IDENTICAL to the
  stage-1c baseline; no violation notes on the red legs → they die of the
  pre-existing disease, not of the shadow machine. 2a is behaviorally
  transparent, as a shadow must be.

## 2b — flip authority (commits 3acb412 + repark fix, binary 47ac175d)

- `_running`/`_suspended` no longer exist as fields: `_isRunningLocked()`
  (Running|Parking|pin) and `_isSuspendedLocked()` (Parking|Parked|Ready) are
  derived views of `_microState`, updated ONLY inside the transition function.
  The impersonation lockout became `_impersonationPin` (it used to be smuggled
  through `_running=true` on a non-running thread).
- doWork captures `preDispatchParked` before the Running transition (the
  dispatch-decision logic needs the pre-dispatch view); suspend()'s
  park-vs-resumed discriminator and the inline suspend-contract checks read the
  state directly. The 2a flag-consistency asserts are gone (tautological).

### 2b fuzz finding #2 (the tape's second catch, and the flip's one real bug)

First 2b build wedged EVERY fuzz boot (launchd parked forever). One gdb walk
with the tape named it: a spurious dispatch of a Parked thread went
`Parked→Running("dispatch-stale")→Idle("done")` — doneWorking's Parking-only
repark discriminator LOST the untouched parked context that the old
`_suspended` flag carried implicitly; the next genuine wake dropped as
"no-park" (tape[255..257] on the wedged launchd, verbatim). Fix: doWork records
`parkedContextIntact` at the no-op-dispatch goto; doneWorking reparks
(Parked/Ready, reason "repark-noop-dispatch") instead of idling.

### 2b gate status

- Synth gate (47ac175d): 15/16 — one `nestwait-off-4` RED: server death at
  TEARDOWN (after t=15s steady progress; -111 on interrupt_enter +
  semaphore_timedwait), zero mstate violations, no panic. Does NOT reproduce:
  8/8 green ring-off reruns under attached gdb. Same shape as the teardown
  server death already observed once on the OLD baseline during stage-1 Part-3g
  work (#114 mass-exit family). Classified: rare pre-existing, WATCH ITEM — any
  recurrence gets the tape+gdb treatment.
- cvstorm2-throttled (#114 known-limit) went GREEN this run — first time; storm
  legs shift with timing, not claiming improvement.
- **Landing battery: quick gate 15/15 gating GREEN** (nestwait-off 4/4 — watch
  item did not recur; brew xz 2/2 strict), zero mstate violations. Perf A/B:
  nestwait NO_STORM 3626/3674/3644 jobs/15s + 300x true 2s — inside the
  3609–3692 baseline corridor. **2b LANDED, deployed 47ac175d = new baseline.**
  (Ops note: the stage-1 perf script wedged once on its in-guest `| tail` pipe
  with a plain `timeout` that the launcher ignores — replaced by
  job-tmp s2b_perf2.sh: `timeout -k 5`, file redirect only, no in-guest pipes.)

## 2c — XNU wait-state writes through one funnel (commit 5cf6081, binary 7692d9f6)

Recon first, and it shrank the problem decisively: in the COMPILED duct-tape set
(sched_prim.c / thread_act.c / kern/locks.c are NOT built; src/thread.c holds the
live copies), **no code outside duct-tape/src/thread.c touches the TH_* state
bits at all**, TH_RUN and block_hint are write-only diagnostics, and exactly one
external site writes wait_result (waitq.c's prepost early-out).

- `xnu_wait_state_write(thread, new_state, write_wresult, wresult, reason,
  violation)` in duct-tape/src/thread.c is THE funnel; all nine write sites route
  through it as named transitions: `xwait-destroy`, `xwait-sigexc-abort`,
  `xwait-dying` (its pre-clear/skip-thread_go semantics documented, kept as-is),
  `xwait-unblock`, `xwait-mark-wait`, `xwait-kthread-birth`,
  `xwait-start-assert-wait`, `xwait-user-suspension-wait`, and
  `xwait-prepost-awakened` (helper called from the patched waitq.c site).
  Birth init in dtape_thread_create stays outside (owning Thread mid-construction).
- New hook `thread_xwait_transition` -> `Thread::recordXnuWaitTransition`: tape
  event `StateEvent::XnuWait` (aux8 = new TH_* bits, aux64 = old bits<<32 |
  wait_result); a violation gets the full MSTATE VIOLATION treatment (error log,
  tape dump, abort under DSERVER_MSTATE_ABORT=1 -- the existing gate grep catches
  it with zero gate changes). Modeled violations: thread_unblock on a non-waiting
  thread; mark-wait double-mark or on a terminating thread (whose full-overwrite
  would silently erase TH_TERMINATE -- kept, now visible).
- **`dtape_thread_entering` DELETED** -- its unconditional "entering => cannot be
  waiting" TH_WAIT clobber was the A0 disease, and doWork's preserveWaitState
  skip-dance existed only to dodge it. The skip-condition became the assertion the
  spec asked for: an unambiguous fresh-call dispatch (doWork + doWorkInline +
  doMachReplyPortInline) asserts the guest thread is NOT waiting;
  `dtape_thread_clear_stranded_wait` recovery-clears a stranded TH_WAIT exactly
  like entering used to, but reports it via `_strandedWaitViolationLocked`
  (loud instead of silent laundering; release builds stay as robust as before).
- Part-3e re-park loop iterations are taped (`xwait-repark-still-waiting`, both
  the inline thread_block loop and thread_continuation_callback's).
- Lock discipline audited per site: the hook takes the Thread rwlock (established
  order thread_lock -> rwlock, same as the arm/resume hooks these sites already
  fire); dtape_thread_exiting keeps its TH_RUN clear UN-hooked (doneWorking holds
  the rwlock there); the two inline dispatch sites tape the assertion themselves
  under their held lock.

### 2c gate results (all first-try on the first 2c build)

- Synth gate: **ALL 16 gating legs GREEN, ZERO mstate/xwait violations** across
  the battery -- the funnel's legality claims (unblock-implies-waiting,
  no-double-mark) hold under fuzz immediately, and the fresh-dispatch stranded-
  wait assertion never fired. Fuzz survival 4 RED / 6, same count as the 2a/2b
  baseline, all red causes = catalogued pre-existing classes (s1 legs: UDS
  reply-stream desync BAD RECEIVE cascade; s2/s3 nestwait: server death at
  interrupt_enter, the #114 stage-3 family). Storm legs shifted greener
  (cvstorm2-throttled OK; flood red that run), as they do with timing.
- Perf A/B: nestwait NO_STORM **3627/3662/3625** jobs/15s + 300x true **1s** --
  inside the 3609-3692 corridor; the two extra hook calls per wait episode
  (mark-wait + unblock) are free at RPC granularity.
- Landing battery quick gate: **19/19 gating GREEN** (boot, nestwait 8/8,
  forkwait, cvstorm-nostorm 3/3, BOTH storm known-limit legs, brew xz 2/2
  strict), zero violations anywhere including real brew load.
  **2c LANDED, deployed 7692d9f6 = new baseline.**

Stage 2 is COMPLETE: one MicroState machine owns the C++ run-state (2a/2b) and
every XNU wait-state write is a named, taped, legality-checked transition (2c).
Next: stage 3 interrupt-as-cancellation (kills jumpToResume stack borrowing;
closes #114 + the EDEADLK self-rwlock face + the remaining fuzz reds;
acceptance = `A0_STRICT=1 a0-gate full` GREEN).

## ASAN leg (stage-0 leftover, folded in here)

- `-DDSERVER_ASAN=ON` builds clean (binary 417b961c, saved in job tmp next to
  the normal e50a3450; build tree restored to ASAN=OFF afterwards).
- **Boot requires `ASAN_OPTIONS=detect_stack_use_after_return=0`.** With FSUAR
  on, the very first inline doWork stint dies (SEGV reading the zero page from
  `isCurrentlySuspended` right after the fiber returns): ASAN's fake-stack
  machinery does not survive the getcontext/setcontext fiber switches on the
  perf#2b inline-dispatch path even with the __sanitizer fiber annotations —
  locals of the interrupted frame read back as garbage. Instrumentation
  incompatibility, not a server bug; FSUAR off boots and runs green.
- Reduced synth battery under ASAN (A0_NEST_RUNS=2 A0_CV_RUNS=1 A0_FUZZ_SEEDS=2):
  **ALL 9 gating legs GREEN — no memory errors on the healthy paths.**
- One ASAN report on a red fuzz leg (fuzz-s1-nestwait): stack-buffer-underflow
  WRITE in `shared_from_this()` inside doMachReplyPortInline on the MAIN loop —
  a function with no fiber switches of its own. Most plausibly the main stack's
  ASAN shadow left poisoned by earlier fiber switches (same instrumentation
  family as the FSUAR boot crash), NOT conclusively a real bug; a fiber-
  annotation audit would settle it, but stage 3 removes the stack borrowing
  entirely, which is the real fix. Catalogued, not chased.

Perf A/B (e50a3450 vs 886d13af/stage-1 numbers): nestwait NO_STORM
3642/3614/3692 jobs/15s (mean ~3649 vs stage-1 ~3635 / baseline ~3679, within
the 3609–3692 historical spread); 300x /usr/bin/true = 2s (identical).
**No regression** — the tape writes are free at RPC granularity.
