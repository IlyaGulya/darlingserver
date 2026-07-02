# perf#24f-live-A/B — BLOCKED: DCC5 crashes guest in early init; capture is the obstacle (task #102)

Status: **A/B BLOCKED.** DCC5-ON crashes EVERY guest process (not just clang — /usr/bin/true too) with
SIGSEGV in libSystem.B's initializer, very early (pre-signal-handler). The RIP-relative rewrite is now
PROVABLY COMPLETE (exec-coverage gate: 0 uncovered executable bytes), so the remaining crash is NOT a
missed code rewrite — it is a separate, non-rewrite defect. Getting its exact fault-PC is blocked by
Darling's process/signal model. Prod restored byte-identical throughout (doctor ALL GREEN).

## What is proven GREEN (offline, committed darlingserver a19caa3)
- exec-coverage: every executable section enumerated from Mach-O metadata; ZERO uncovered exec bytes
  (4,426,825 bytes). __text (llvm-objdump) + __stubs (2859, format scan) + __stub_helper (80 rip sites,
  format scan). Unknown byte in an exec section => hard abort.
- All rip-rel disp32 (text 23182 + stubs 2859 + helper 80) rewritten & verified to land in the moved
  region; determinism md5-stable; all RED arms detected (incl. no-stubs / no-helper / unknown-pattern).
- So: the code rewrite is complete and correct. The crash is elsewhere.

## The crash (corrected understanding)
- FALSE PASS in task #101: the live-smoke "exit 0" read the LAUNCHER rc, not the guest-child rc. With
  the guest-child rc checked (`sh -c '...; echo RC=$?'`), /usr/bin/true AND clang both return 139 (SIGSEGV)
  under DCC5 ON. The rewrite fixed the gross "__text" instruction-fetch crash (no more 0x747865 fault),
  but a different early-init crash remains.
- dyld log shows: reader enabled, 40 images substituted, 27082 fixups applied once, then
  "calling initializer function 0x…cc0 in libSystem.B" — crash INSIDE that initializer.

## Why every fault-PC capture attempt failed (the real obstacle)
1. crashcap-sigexc.patch (guest libsystem_kernel SIGSEGV handler): 0 CRASHCAP lines. The crash happens
   BEFORE the guest installs its signal handler, so sigexc never runs. (Built instrumented
   libsystem_kernel @a0328833, baked into cache, deployed — still nothing.)
2. strace -f on the launcher: 0 SIGSEGV. The guest mldr is spawned by darlingserver/shellspawn, NOT a
   fork-child of the `darling shell` launcher — strace can't follow into it.
3. gdb-launch mldr directly: "Unknown file format: /usr/bin/true" — mldr needs the launcher's vchroot
   setup to resolve guest paths; direct launch bypasses it.
4. gdb the launcher with follow-fork: followed the launcher's own forks (which exit normally); the
   crashing guest mldr is in darlingserver's tree, not the launcher's.
5. gdb attach to shellspawn (the actual guest-spawner) with follow-fork-mode child: followed the fork
   into the guest mldr, but gdb reports "Target and debugger are in different PID namespaces …
   Connect to gdbserver inside the container" and the followed inferior "exited normally" — Darling
   TRANSLATES the guest SIGSEGV into a Mach exception before a host-observable SIGSEGV, so host gdb sees
   a clean exit, not a fault.
6. Static classification of the recurring dserver symptom "procmem: Failed to write 92 byte(s) at
   arena+0x4278 … No such process": those addresses (arena+0x40d8/0x40f8/0x4278/0x6e18) are all in the
   RX region (libSystem.B __TEXT), but this is AFTERMATH — dserver's post-crash signal/exception write to
   an already-dead process (ESRCH). Not the cause. (The VA-collision hypothesis is not confirmed: the
   write only fails because the process is already gone.)

## The capture path that remains (per gdb's own hint)
Run **gdbserver INSIDE the Darling container** (same PID namespace as the guest), attached to the guest
process, so the guest SIGSEGV is observed before Darling's Mach-exception translation. That needs a
guest-side gdbserver (or a Darling debug hook) — infrastructure this bead does not yet have. Alternatives:
(a) a Darling mldr/dserver debug env that dumps the guest thread state on the Mach EXC_BAD_ACCESS it
already receives (dserver HAS the crashed thread state — it just logs "process died"; extend that log to
dump RIP+fault addr from the exception message); (b) bisect the cached set (1 dylib → grow) to localize
which cached image's init crashes, narrowing without a PC.

## Decision needed
The code-rewrite half of DCC5 is complete and gated. The remaining early-init crash is a genuine defect
whose diagnosis is blocked on capturing a fault-PC through Darling's exception-translation layer. This
needs either a container-side debugger or a dserver exception-state dump — a distinct piece of work.
