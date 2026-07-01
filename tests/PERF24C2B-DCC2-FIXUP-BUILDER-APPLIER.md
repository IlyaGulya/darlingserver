# perf#24c2b — DCC2 fixup-table builder + standalone applier validator (no live dyld)

**Outcome: GREEN.** The DCC2 builder generates a cache-native precomputed fixup table, and a standalone
applier applies it to the packed regions so the result matches normal-dyld expectations — **without invoking
dyld**. All 7 acceptance checks pass on both the 2-dylib smoke cache and the full 40-dylib closure; all 4 RED
arms are detected. No dyld code was written, nothing deployed. prod dserver `835946f9` + mldr `f0cd2a82` +
dyld `10af572e` UNTOUCHED. LOCAL only.

## New invariant (carried into c2c/c2d)
**DCC images must NEVER enter the normal Mach-O fixup path.** If a DCC image reaches
`applyFixupsToImage → WrappedMachO::forEachFixup`, that is a hard fail/assert/log — the normal path is
known-wrong for region-packed layout (perf#24c2a red line). DCC2 replaces it entirely with the cache-native
table.

## DCC2 format (`tools/closure-cache/dcc2-format.h`, magic `DCC2`, version 2)
DCC1 (region-packed, rewritten header vmaddrs) **plus** a fixup table:
```
header{ …DCC1 fields…, fixup_off, fixup_count, extern_off, extern_size }
image_table[]{ …, fixup_first, fixup_count, segs[] }
dcc_fixup{ kind, loc_region, tgt_region, loc_off, tgt_off, addend, extern_sym, image_index }
extern strtab (flat/unresolvable symbol names for the runtime resolver)
```
All fixup locations and internal-bind targets are **cache-relative (region id + offset)** so one uniform
runtime slide relocates everything:
- `DCC_REBASE`        → `*(RW+loc_off) += slide`
- `DCC_BIND_INTERNAL` → `*(RW+loc_off) = region[tgt_region].vm_base + tgt_off + slide + addend`
- `DCC_BIND_EXTERN`   → `*(RW+loc_off) = resolver(extern_sym) + addend` (runtime dyld symbol lookup)

## Builder (`dcc2-builder.c`)
Region-packs (RX/RW/RO) + rewrites `LC_SEGMENT_64.vmaddr` in the packed header (perf#24c2a fix), then walks
the opcode rebase/bind streams (closure is 100% `LC_DYLD_INFO_ONLY`, zero chained — per audit) and emits the
table. **Bind resolution at build time**, resolved to cache-relative targets:
- two-level (`ord≥1`): look up the symbol in the dep dylib's **export trie** → cache offset.
- self (`ord==0`): look up in the image's own trie, then **follow its `LC_REEXPORT_DYLIB` deps** — this was the
  key fix: umbrella libs like **libSystem.B re-export their 33 sub-dylibs' whole export sets implicitly** (via
  `LC_REEXPORT_DYLIB`, not explicit re-export trie nodes), so `_malloc`/`_free`/`_write`/… self-binds resolve
  into the sub-dylibs.
- flat (`ord==-2`): search all images; resolve if a definer is present, else record as **EXTERN** (never
  silently dropped).

### Counts (match the perf#24c2a audit exactly)
- **Full 40-dylib closure:** 24,802 fixups = **23,407 rebases + 1,395 binds**; binds = 1,236 two-level +
  154 self + 5 flat-resolved + **0 extern**, **0 unresolved**. Every bind resolves inside the closure.
- **2-dylib smoke {libsystem_blocks, libunwind}:** 55 fixups = 50 rebases + 5 binds; 1 flat-resolved in-set +
  **4 extern** (genuinely external flat: `dyld_stub_binder`, `__NSConcreteMallocBlock`, `___stack_chk_guard`,
  `___stderrp`). These 4 are the "5 flat binds as their own gate line" — correctly bucketed for the runtime
  resolver, not dropped.

## Standalone applier / validator (`dcc2-applier.c`)
Maps the 3 regions preserving fixed relative vm-base spacing (one uniform slide, RW as `MAP_PRIVATE`/COW),
applies the whole table to the private RW copy, and runs:
1. parse all images ✓
2. table counts + classification (rebases/binds; unresolved=0 or explicit extern) ✓
3. apply fixups to a private mapped copy of packed DATA ✓ (full: 24,802 applied)
4. every fixup **location** lands inside the RW region ✓
5. every internal-bind **target** lands inside a packed region; every extern references the resolver strtab ✓
6. normal dyld fixup engine no longer needed — every fixup attributed to an image, ranges contiguous, table
   fully covers DCC images ✓
7. deterministic — byte-identical rebuild (full md5 `ee17e2e3…`, smoke `aefded8e…`), atomic (tmp+rename) ✓

Plus: spot-check that applied rebases point within the cache arena (200/200 full); **COW proof** (writes to
private RW did not modify the cache file); **byte-identity vs source** — DATA/LINKEDIT/TEXT-body all identical
across 120 segments, only header `LC_SEGMENT_64.vmaddr` intentionally changed.

### RED arms (each MUST be detected — all PASS)
1. fixup **location computed from OLD image offset** → lands outside RW region ✓ detected
2. **target using ORIGINAL inter-segment delta** → target outside any region ✓ detected
3. **bind target outside closure without resolver** → unresolved, no extern bucket ✓ detected
4. **stale header vmaddr** → header vmaddr ≠ table vmaddr ✓ detected

## Next
perf#24c2c: flag-gated dyld DCC2 path + 2-dylib live smoke {libsystem_blocks, libunwind}. Only after this
green c2b. The reader maps 3 regions, sets loadedAddress, `setState(fixedUp)`, **skips** `applyFixupsToImage`,
and applies the cache-native table; the extern bucket (the 4 flat symbols) goes through the normal dyld symbol
resolver. Enforce the new invariant (DCC image never enters the normal fixup path = hard fail).

## Artifacts
`tools/closure-cache/`: `dcc2-format.h`, `dcc2-builder.c`, `dcc2-applier.c`, `bindnames.c`, `byteid.c`,
`smoke2-list.txt`, `DCC2-BUILD-VALIDATE-OUT.txt`. Reuses perf#24c2a audit (`audit/`).
