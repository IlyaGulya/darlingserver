# A0-ARCH stage 2: one run-state machine per thread (kills the dual-view disease)

Status: **IN PROGRESS** (2026-07-04). Branch `fix/a0-arch-redesign`.
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

Probe pass (e50a3450, abort OFF to collect ALL violations per run):
(pending)

## 2b — flip authority (planned)

- `_running`/`_suspended` become derived views (accessors over `_microState`);
  every flip site becomes a named transition; the impersonation lockout becomes
  its own `_impersonationPin` so "Running" regains a single meaning.
- Negative-direction consistency asserts turn on (Parked/Ready ⇒ NOT running).

## 2c — XNU side becomes derived (planned)

- `TH_WAIT`/`wait_result` writes at thread_mark_wait_locked / thread_unblock /
  clear_wait_internal / dtape_thread_dying route through named transitions;
  `dtape_thread_entering` DELETES (the preserveWaitState hack disappears);
  Part-3 skip-conditions become assertions.

## ASAN leg (stage-0 leftover, folded in here)

(pending — -DDSERVER_ASAN=ON build of 2a through a short synth battery)
