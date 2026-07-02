# perf#24e — x86_64 RIP-relative rewrite feasibility (offline spike)

Status: **NEAR-GO with one bounded caveat.** Decides whether the only path to a real VMA collapse
(114 -> ~3) — the tight 3-region DCC cache + TEXT code rewrite (Apple's shared-cache approach) — is
provably complete and safe for the 40-dylib closure. Offline only; no dyld built, nothing deployed,
prod untouched.

## Why this is the only path (from perf#24c2d-fix2 / task #98)
The impossible triangle is proven: {no code rewrite} + {exact x86_64 semantics} + {big VMA collapse}
cannot all hold. Delta-exact rigid slabs = 120 VMAs (zero collapse); region-packing that collapses
VMAs breaks baked RIP-relative TEXT->DATA displacements = the "__text" crash (root cause task #97).
So a real win REQUIRES rewriting the RIP-relative code references.

## Method
Primary decoder = `llvm-objdump -d --arch=x86_64` (complete), NOT LINKEDIT (general GOTPCREL/local
refs have no dynamic reloc metadata). Enumerate every RIP-relative disp32 in `__text` across all 40
dylibs; classify each target; simulate a tight 3-region layout (RX=all TEXT, RW=all DATA, RO=all
LINKEDIT, one arena, non-overlapping); compute the rewritten disp32 and check signed-int32 fit.
Tools (committed): `dcc-riprelscan.py`, `dcc-decode-coverage.py`, `dcc4-packsim.py`.

## Results
- **42002** RIP-relative disp32 sites in `__text` closure-wide.
  - `same-TEXT` **18636** — target stays in the same image's __TEXT (moves rigidly with it) => disp
    UNCHANGED, no rewrite needed, automatically correct.
  - `same-DATA(RW)` **23182** — target crosses to the moved RW region => needs rewrite. All computed;
    **INT32 overflow after rewrite = 0** (arena ~9MB, far inside +/-2GB).
  - `same-LINKEDIT` 0, cross-image 0, truly-outside-any-segment **0** (the earlier "184 outside-image"
    were targets in segment padding between sections — still in-segment, move rigidly, safe).
- **15430 KIND_JUMP_TABLE32** entries embedded in __text (compiler switch tables): **0 target outside
  __TEXT** — all TEXT-internal, safe under a rigid TEXT move, no rewrite needed.
- **Decode coverage of __text: 100% except 24 bytes in ONE dylib (libsystem_m).** All other 39 dylibs
  decode with zero non-padding gaps (gaps are cc/00/90 alignment padding).

## The one caveat (GO criterion #6: "no unknown instruction sites")
`libsystem_m.dylib` embeds a floating-point CONSTANT POOL inside `__text` (24 bytes llvm-objdump
flags as "bad opcode" — they are data, not instructions: double bit-patterns like 0x4027173e, typical
of a hand-tuned libm). A pure decoder cannot distinguish data-in-text from code with certainty, so a
naive code-rewriter risks (a) missing a real rip-rel site hidden in apparent data, or (b) corrupting a
constant pool whose bytes coincidentally look like `disp(%rip)`. This is exactly the "not provably
complete" condition. BUT it is a SINGLE, BOUNDED, well-understood outlier (math library), not a
pervasive failure: 39/40 dylibs are 100% clean.

## Verdict options
- **Strict GO** is NOT met as-is (libsystem_m has unknown data-in-text). But the risk is isolated and
  handleable three ways: (1) special-case libsystem_m — keep it as a per-image slab (its own 3 VMAs)
  and rewrite only the 39 clean dylibs (net VMAs ~= 3 regions for 39 + 3 for libsystem_m = ~6, still a
  huge win vs 114); (2) obtain function-boundary info (LC_FUNCTION_STARTS) to bound __text code vs the
  constant pool precisely, then rewrite only within function ranges; (3) drop libsystem_m from the
  closure cache entirely (it's a leaf math lib, rarely on the hot launch path).
- **NO-GO** if we require a single uniform rewriter with zero special-casing and zero function-boundary
  input.

RECOMMENDATION: conditional GO via option (1)+(2) — LC_FUNCTION_STARTS gives exact code ranges so the
constant pool is provably excluded, making the rewrite complete AND safe. Then perf#24f = 3-region
builder + TEXT rewrite (rewrite the 23182 same-DATA sites; leave same-TEXT + jump tables untouched) +
DCC fixups + live 2-dylib smoke + full closure A/B.

## RED arms (to implement in perf#24f's builder self-check)
old tight 3-region cache must FAIL riprelscan (DATA targets escape); intentionally-skipped GOTPCREL
load must fail; intentionally-wrong new disp32 must fail; target into "__text" section-table bytes
must fail; target outside signed disp32 range must fail.
