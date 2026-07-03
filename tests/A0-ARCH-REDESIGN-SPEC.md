# A0-ARCH: darlingserver wake/wait protocol redesign — task spec

Status: SPEC (not started). Owner: unassigned. Parent context: perf#25a / task #113 (A0
zero-hang fixes, commit a3de8c2) + task #114 (interrupt-window stack-lifetime crash).
Read `tests/PERF25A-BREW-REINSTALL-CENSUS.md` UPDATE 24–26 first — it documents every
measured failure mode this redesign must make *structurally impossible*.

## Why (one paragraph)

The A0 hang family (brew reinstall freezes) was ONE disease with six faces: duct-tape
mirrors XNU's scheduler contract (TH_WAIT / waitq links / wait_result) on top of a
hand-rolled fiber engine with its OWN parallel state (`_suspended` / `_resumePermit` /
`_running` / `_pendingCall`), and nothing but comments keeps the two views coherent.
Every face was "one half updated, the other not". The a3de8c2 fixes patch each measured
face; this redesign removes the CLASS. A seventh face is still open (#114: interrupt
fiber frames abandoned on stacks that get freed/reused → SIGSEGV under pthread_kill
storms at contended psynch condvars) and is expected to fall out of stage 3 below.

## Hard constraints (non-negotiable)

1. **Zero hangs stays zero.** `tests/a0-repro/a0-gate.sh full` must be GREEN after every
   stage. The gate is the acceptance authority, not code review.
2. **Performance must not regress.** Measured baselines on 886d13af (2026-07-03, this
   host, ring-ON): nestwait `-DNO_STORM` ≈ 3660–3690 jobs/15s; 300× `/usr/bin/true`
   ≈ 1–2 s; `brew reinstall xz` ≈ 66–78 s wall. Re-measure A/B (same-day, same host,
   deploy-swap method — see job-tmp `a0_perf_ab` pattern) after every stage; >3% wall
   regression on any of the three = stop and investigate.
3. **Ring transport + dyld DCC stay untouched** (they live outside the redesigned layer;
   `_publishReplyToRingLocked` / `beginRingReply` semantics must be preserved exactly).
4. Workspace discipline: build ONLY in `~/work/darling-build`; `DARLING_SKIP_DOCTOR=1`
   for intentional-drift deploys; hard teardown between boots (kill launchd/mldr/
   darlingserver/vchroot/shellspawn + rm socket + rm workdir — a leftover launchd
   silently blocks shellspawn); deployed-binary baseline is doctor-checked
   (`deploy-baseline.md5`, currently dserver=886d13af — bump it when a stage lands).
5. Small stages, each independently shippable and gated. NO big-bang rewrite.

## Current architecture (what you are changing)

- `src/thread.cpp` — the fiber engine: `doWork()` (dispatch; the "major UB" function),
  `suspend()/resume()` (`_resumePermit` = ONE untyped bool token), `microthreadWorker`,
  `_handleInterruptEnterForCurrentThread` (interrupt stacking: getcontext/jumpToResume
  stack jumps, `_interrupts` stack, savedStack juggling), `doneWorking` tail
  (`_rerunPending`, `_pendingInterrupts` promotion).
- `duct-tape/src/thread.c` — the XNU-side glue: `thread_block_parameter` (+ Part 3e
  re-park loops), `thread_unblock` (Part 2a finalization), `clear_wait_internal`,
  `dtape_thread_entering` (Part 3 skip conditions), `dtape_thread_sigexc_enter`,
  hooks (`thread_clear_resume_permit`, Part 3d).
- `duct-tape/src/locks.c` / `condvar.c` — RAW suspend/resume primitives (kwq lock
  queues) that share `_resumePermit` with XNU waits — the crosstalk source.
- Two fiber runners exist despite `DSERVER_SINGLE_THREADED=1`: the main event loop runs
  `doWork()` inline (perf#2b) AND one WorkQueue worker thread runs it too; the ring
  service and timer paths also touch Thread state. TLS (`currentThreadVar`) is
  per-OS-thread and fibers migrate between runners.

## Target architecture

### Stage 1 — typed wake tokens (kills permit crosstalk residue)

Replace `bool _resumePermit` with a token: `{ uint64_t generation; enum WakeKind
{ XnuWaitFinalized, RawHandoff, InterruptResume } kind; }`.

- Each wait registers a generation (per-thread monotonic counter) when it parks.
- `resume()` callers state the kind + the generation they are waking (thread_unblock
  knows it — it just finalized that wait; dtape_mutex_unlock knows its queue link).
- `suspend()` / `thread_block` consume ONLY a matching token; a mismatched token is
  logged (debug: assert) and dropped — a stale wake becomes a detectable no-op instead
  of a spurious wakeup. This retroactively subsumes Parts 3b/3c/3d (keep them; they
  become dead-in-practice safety nets) and makes Part 3e's re-park loop the rare path
  it was meant to be.
- Gate after stage: full a0-gate + `A0_STRICT=1` storm legs (expected: still red — the
  crash is stack-lifetime, stage 3 — but must be *no worse*: record survival counts).

### Stage 2 — one state machine per thread (kills the dual-view disease)

Introduce a single authoritative per-thread run-state:
`enum RunState { Running, Parked(kind, generation), Ready(wait_result) }`
with ONE transition function that asserts legality (e.g. Parked→Ready only via
finalize-with-result; Ready→Running only by a dispatch; Running→Parked only by self).

- `TH_WAIT`/`wait_result` on the XNU side and `_suspended` on the C++ side become
  DERIVED views (read-only accessors) of this one field, updated inside the transition
  function only. `dtape_thread_entering`'s "entering ⇒ cannot be waiting" DELETES —
  the state machine makes the question well-posed instead.
- Every site that today hand-flips bits (`thread_unblock`, `clear_wait_internal`,
  `thread_mark_wait_locked`, `dtape_thread_dying`, doWork's preserveWaitState block)
  is rewritten as a named transition. The Part 3 skip-conditions disappear as code and
  become assertions.
- Debug build: transition log ring-buffer per thread (last 16 transitions) dumped on
  panic — replaces ad-hoc printf archaeology.
- Gate after stage: full a0-gate; perf A/B (the transition function is hot-path — keep
  it a switch + store, no locks beyond what exists today).

### Stage 3 — interrupt processing without stack jumps (closes #114)

Kill `jumpToResume`/`getcontext(_syscallReturnHereDuringInterrupt)` stack acrobatics:

- Model an interrupt as a CANCELLATION REQUEST on the parked call: sigexc_enter
  finalizes the wait as INTERRUPTED via the (stage 2) transition function; the
  interrupted call resumes through the NORMAL dispatch path (its own fiber, its own
  stack) and unwinds; `interrupt_enter`'s reply is sent when the interrupted call's
  reply has been stashed (a small completion callback / semaphore — same ordering the
  guest protocol needs, without borrowing the interrupted call's stack).
- Fiber stacks get single ownership: a stack is freed exactly once, by the fiber's own
  completion, never by `_interrupts.top().savedStack` bookkeeping from another frame.
  If refcounting is simpler than ownership transfer, refcount — but prefer "the stack
  dies with the fiber".
- FALSIFIED already (do NOT retry as-is): Part 3g "force-finalize the interrupted wait
  before resuming the continuation" — reverted; the abandoned frame comes from the
  psynch continuation re-taking the kwq lock and raw-suspending mid-unwind, not from
  the 3e re-park; and the TH_WAIT+waitq==NULL hand-finalize corrupted mid-assert IPC
  waits (one nestwait teardown-phase server death observed).
- Acceptance for this stage: `A0_STRICT=1 tests/a0-repro/a0-gate.sh full` GREEN —
  i.e. the cvstorm2 throttled AND flood legs stop crashing. That is the #114 close-out
  criterion. Repro dies in seconds: `cvstorm2.c -DNCONS=1 [-DSTORM_THROTTLE_US=1000]`;
  gdb capture harness pattern is in the census doc (UPDATE 26 amendment).

### Stage 4 — true single-runner event loop (optional, biggest win if taken)

Make "single-threaded" true: all Thread/Process state mutated ONLY from the main loop;
the WorkQueue worker is deleted (or demoted to running strictly non-state-touching
work); ring service and timer threads post closures to the loop instead of touching
state. TLS `currentThreadVar` stops being a correctness hazard because fibers never
migrate. Measure first: perf#2b showed the worker handoff was ~70% overhead for cheap
RPCs — inlining everything on the main loop may WIN perf, but a long-running microthread
then blocks the loop; needs a budget/yield policy. If the numbers do not clearly
support it, stop after stage 3 and write down why.

## Cross-cutting: verification you must add (cheap, do these first)

- **Yield fuzzer**: under `DSERVER_SCHED_FUZZ=<seed>`, doWork/suspend inject random
  re-dispatch orderings (e.g. 1-in-N chance to defer a dispatch or re-run doneWorking's
  reschedule). Every A0 face was a 2-event reorder; a seeded fuzzer finds this class in
  minutes. Wire a fuzz leg into a0-gate (`synth` mode, N seeds).
- **ASAN gate**: `DSERVER_ASAN=1` build (support exists) through a0-gate `synth` —
  catches stack reuse (#114 class) deterministically instead of via corrupted-frame
  SIGSEGVs. Note ASAN fiber annotations are already present in thread.cpp.
- Keep (and extend) the zero-cost-when-quiet diagnostics from a3de8c2: panic() host
  backtraces, enriched waitq/semaphore panic context, `DARLING_SERVER_AUXLOG=1` RPC
  tape. Every new invariant = an assert in debug, a logged-and-tolerated event in
  release.

## Deliverables

1. Stages 1–3 landed as separate commits on a `fix/a0-arch-*` branch, each with its
   gate + perf A/B results recorded in a `tests/A0-ARCH-<stage>.md` note.
2. `A0_STRICT=1` full gate GREEN (closes #114) after stage 3.
3. deploy-baseline.md5 bumped per landed stage; manifest handoff refreshed.
4. Stage 4: measurement + go/no-go writeup even if not implemented.
