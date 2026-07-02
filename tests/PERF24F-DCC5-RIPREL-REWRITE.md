# perf#24f — DCC5: 3-region cache + TEXT RIP-relative rewrite (offline gates GREEN)

Status: **OFFLINE GATES ALL GREEN.** The only path to a real VMA collapse (114 -> 3) is proven complete,
safe, and deterministic offline. Live boot sequence is the next phase (explicitly gated behind these
offline gates). Nothing deployed; prod byte-identical throughout (doctor ALL GREEN).

## Why this path (recap of the impossible triangle)
- DCC2 (3-region pack, perf#24c2d): collapses VMAs to 3 but a POINTER fixup table cannot touch
  instruction immediates => every baked x86-64 RIP-relative TEXT->DATA disp32 (stubs, GOTPCREL, local
  refs) is left stale => the "__text" instruction-fetch SIGSEGV (root cause task #97).
- DCC4 (delta-exact unified arena, perf#24c2d-fix2): preserves the disps but forces perms to cycle every
  image => 120 VMAs = zero collapse (task #98 dead end).
- perf#24e: proved the rewrite is bounded+safe (42002 sites: 23182 same-DATA need rewrite, all int32-safe;
  18636 same-TEXT + jump tables move rigidly; one libsystem_m data-in-text outlier).
- **perf#24f (this): 3-region layout (real collapse) + REWRITE the 23182 same-DATA disp32 at build time.**

## Rewrite math (per rip-rel site)
Site at original vmaddr V, length ilen, disp field disp, target T = V+ilen+disp. Under the 3-region
relayout each segment S moves by segDelta(S) = (packed seg vmaddr) - (orig seg vmaddr).
    newdisp = disp + segDelta(tgt_seg) - segDelta(__TEXT)
- same-TEXT (segDelta equal): newdisp == disp => LEFT UNCHANGED (no patch). 18820 sites.
- same-DATA/RO (segDelta differ): rewrite the disp32 in the packed RX bytes. 23182 sites. All fit int32.
The disp32 field is located inside the instruction's raw bytes by its unique little-endian value
(verified closure-wide: exactly one match per instruction even for the 2010 sites carrying a trailing
immediate; >1 match => hard abort, never a guess).

## Tools (committed)
- `dcc5-format.h` / `dcc5-builder.c` — DCC5 = DCC2 3-region layout + fixup table + the build-time rip-rel
  rewrite pass. Decoder = `llvm-objdump -d --arch=x86_64 --no-symbolic-operands` (perf#24e source of truth).
- `dcc5-riprelgate.py` — AFTER-REWRITE gate: overlays the packed __text onto a copy of each dylib,
  re-disassembles with llvm-objdump, and verifies every rip-rel target lands at its correct packed
  address (same-TEXT stays in the image __TEXT; same-DATA lands at the exact packed RW/RO offset).
  Independent of the builder's disp-finding => real teeth.
- `dcc5-redarms.py` — RED arms 2/3/4 (cache mutations). `--red5` builder flag = RED arm 5.
- `dcc5-gates.sh` — consolidated runner (all hard gates + all 5 RED arms).

## Build result (40-dylib closure, /home/ilyagulya/work/darling-prefix)
```
RX=0x52c000 RW=0xfc000 RO=0x2a0000 total=0x9dc000
fixups=27082 rebases=23407 binds=3664 [internal=3452 self=179 flat=33 extern-lazy=11] unresolved(suspicious)=0
RIPREL: total=42002 rewritten(same-DATA/RO)=23182 same-TEXT(left)=18820 skipped-pool=0
        [int32-overflow=0 outside-seg=0 pool-overlap=0]
constant-pool (data-in-text) bytes protected=24 (never rewritten)
```
Counts match perf#24e exactly (23182 rewrite reproduced by an independent Python replica and the scanner).

## HARD GATES — all GREEN
1. **libsystem_m data-in-text**: 24 undecodable non-padding __text bytes (FP constant pool). CORRECTION
   to perf#24e: they lie INSIDE function ranges (LC_FUNCTION_STARTS marks entry points only, and constant
   islands sit mid-function), so a function-bounds filter does NOT exclude them. The real protection is
   **decoder omission**: llvm-objdump never emits a `(%rip)` operand for pool bytes, so they never enter
   the rewrite loop. Made explicit + testable: the builder computes the undecodable-pool byte bitmap and
   HARD-ABORTS if any disp32 write would overlap a pool byte. Measured: libsystem_m rewrite changed 5 bytes,
   touched 0 of the 24 pool bytes.
2. **Rewrite coverage complete**: after-rewrite gate re-decodes all 42002 sites from the packed cache;
   23182 same-DATA/RO resolve to the exact packed target, 18820 same-TEXT stay inside the image __TEXT,
   0 mismatch, 0 escape.
3. **No unresolved classes**: int32-overflow=0, outside-seg=0, cross-image=0, pool-overlap=0, unresolved
   binds(suspicious)=0.
4. **same-DATA rewritten / same-TEXT unchanged**: subsumed in gate 2 (both classes checked exactly).
5. **Determinism**: rebuilds -> identical md5 `2b9e0dd2966681a279ac8ce05c263144` (dcc_image kept
   byte-identical to DCC2's 768-byte layout so the dyld reader struct matches exactly; per-image riprel
   counts live in build-time counters + the header totals, not on disk).

## RED ARMS — all DETECTED
1. Unrewritten DCC2 fed to the rewrite gate -> 23182 mismatches, gate FAIL (exit 1). This is the actual
   crash class the gate must catch.
2. Revert one rewritten same-DATA disp32 to original ("skipped rewrite") -> gate FAIL.
3. Corrupt one rewritten disp32 by +0x10 ("wrong disp") -> gate FAIL.
4. Rewrite a same-TEXT site as if it targeted moved DATA (jump-table / TEXT-internal treated as data)
   -> gate FAIL (escape/mismatch).
5. `--red5`: force a rewrite onto a libsystem_m pool byte ("pool treated as code") -> builder ABORT.

## Projected VMA collapse
DCC5 reader maps EXACTLY 3 VMAs for the whole 40-dylib closure (RX 5296KB / RW 1008KB / RO 2688KB) vs
~114 today (38-40 dylibs x 3 segments). Projected per-process **114 -> 3**. This is the real collapse
DCC4 could not reach (delta-exact => 120). Fault-in bytes unchanged (same content); win = VMA create +
teardown (exit_mmap, perf#22a 88% bucket) + dyld load.

## NEXT (live phase, gated behind these gates)
Resurrect the DCC5 3-region reader (fix/dyld-dcc2-reader @02696ac already has the 3-region mapping;
point it at DCC5, skip normal fixups, apply the fixup table, MAP the 3 regions with region perms).
Build dyld with xnu@a0328833 (then restore perf/shmem-ring-guest, rebuild dyld static libsystem_kernel).
Deploy both closure copies + cache to guest /private/var/root/full.dcc5. Sequence: 2-dylib smoke ->
full closure /usr/bin/true -> shellspawn -> clang 1-file -> clang 200-file -j8 A/B. Metrics: closure VMAs
~114 -> ~3; exit_mmap per proc; dyld load time; kernel-mm bucket; wall/user/sys; minor faults (~unchanged).
