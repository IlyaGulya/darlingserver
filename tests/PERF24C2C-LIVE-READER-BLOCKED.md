# perf#24c2c — flag-gated live dyld DCC2 reader: implemented + BLOCKED on tree/dylib skew

**Status: reader implemented and building; live smoke BLOCKED by a pre-existing dyld-source ↔ deployed-dylib
version skew (NOT by the DCC2 code).** Environment left healthy: baseline dyld restored and booting; prod
dserver `835946f9` + mldr `f0cd2a82` untouched. Reader code preserved (uncommitted, flag-gated, inert when
the flag is absent). No live smoke run.

## What was implemented (compiles + links into dyld)
Flag-gated DCC2 reader wired into the dyld3 launch path:
- `dyld3/DCC2Reader.{h,cpp}` — maps the 3 packed regions into one arena (RX+RO `MAP_SHARED`, RW
  `MAP_PRIVATE`/COW) preserving fixed relative vm-base spacing (one uniform slide), validates
  magic/version + per-image staleness (inode/mtime/size), and applies the cache-native fixup table
  (`REBASE`/`BIND_INTERNAL`/`BIND_EXTERN`).
- `dyld3/Loading.cpp` (`mapAndFixupAllImages`): flag-gated branch — for a DCC2-cached image, set
  `loadedAddress = arena + image_vmbase`, `state=mapped`, then apply the DCC2 table (extern/flat symbols
  resolved via `findExportedSymbol` over the already-loaded images; no silent NULL → hard fail).
- **Invariant enforced:** `applyFixupsToImage` hard-`halt`s if a DCC2 image ever reaches the normal Mach-O
  fixup engine (known-wrong for region-packed layout).
- `src/dyld2.cpp` (`launchWithClosure`): `loader.setDCC2Reader(DCC2Reader::init(envp, log))`.
- Feature gate: `DARLING_DYLD_DCC2=1` + `DARLING_DYLD_DCC2_PATH`. Flag absent ⇒ `init` returns `nullptr`
  immediately ⇒ exact old behavior. Present + broken ⇒ `dyld::halt` (or clean fallback with
  `DARLING_DYLD_DCC2_SOFT=1`). Never half-cache/half-normal.

Build fixes along the way: `_simple_getenv(const char* envp[], …)` signature; `hasExportedSymbol` is
`#if BUILDING_LIBDYLD`-only so it isn't in dyld → switched the extern resolver to
`MachOAnalyzer::findExportedSymbol`; hoisted a nested block into a `Loader::dcc2ResolveExtern` method to
avoid an un-emitted `_block_invoke` link error. Final dyld built clean (`dfd331aa`).

## The blocker (isolated by a clean A/B)
Deploying a freshly-built dyld and booting fails: **shellspawn never comes up** (silent early-boot hang, no
`dyld`/halt line in `dserver.log`). Bisected:

| dyld binary | source | boots the deployed prefix? |
|---|---|---|
| `10af572e` (deployed baseline) | older tree that also produced the deployed dylibs | **YES** (`baseline-boots-ok`, `env-healthy-baseline`) |
| `d4ca8c3f` (pristine current tree, **zero DCC2 edits**) | dyld submodule HEAD `a9c2e29` | **NO** (shellspawn hang) |
| `dfd331aa` (my DCC2 reader) | `a9c2e29` + DCC2 edits | **NO** (same hang) |

Since the **pristine current-tree dyld with none of my changes** fails identically, **the DCC2 reader is not
the cause.** Root cause = version skew:
- deployed closure dylibs (e.g. `libsystem_blocks.dylib`, `libunwind.dylib`) are dated **2026-06-15**;
- the checked-out dyld submodule HEAD `a9c2e29` is dated **2026-06-09** (super-project HEAD `e82a69150` is
  2026-07-01).
- The working baseline `10af572e` was built from whatever produced the 06-15 dylibs — **newer than the
  committed dyld submodule** — so rebuilding *only* dyld from the current checkout yields a loader that is
  incompatible with the deployed dylib closure and silently hangs early boot.

(A confounding factor also seen: repeated boot/kill churn wedges the guest — a stuck `darlingserver`+`launchd`
pair at `ep_poll` with no shellspawn, the perf#23a failure mode. Cleared by killing the wedged pair by PID;
baseline then boots cleanly. This is separate from the skew but made the first A/B runs noisy.)

## Resolution path (for the next step — do this BEFORE retrying the live smoke)
The live DCC2 smoke needs a **matching dyld + dylib closure**. Options, cheapest first:
1. **Rebuild the whole closure consistently and redeploy** the dylibs the smoke touches (or the full
   `libexec/darling` tree) from the current tree, so dyld `d4ca8c3f`/`dfd331aa` and the dylibs agree — then
   verify a plain `d4ca8c3f` boots (no flag) before enabling DCC2. This is the correct fix but is a larger
   deploy than "just dyld."
2. **Build dyld from the source state that produced `10af572e`** (align the dyld submodule / branch to what
   the deployed 06-15 dylibs were built from), apply the DCC2 patch there, deploy only dyld.
3. Confirm which branch/commit the deployed environment was built from (memory notes mldr came from
   super-project `integration/homebrew`; the dyld submodule pointer there may differ from `a9c2e29`).

Until the loader/closure are version-matched, a rebuilt dyld cannot boot this prefix, so the live smoke
cannot run regardless of DCC2 correctness. The DCC2 mechanism itself is already proven correct offline
(perf#24c2b: builder + standalone applier, 7 acceptance checks + 4 RED arms green on the same
{libsystem_blocks, libunwind} pair).

## Artifacts / state
Reader code: `src/external/dyld/dyld3/DCC2Reader.{h,cpp}`, edits in `dyld3/Loading.{h,cpp}`, `src/dyld2.cpp`,
`CMakeLists.txt` (uncommitted in the dyld submodule working tree). prod dserver `835946f9` + mldr `f0cd2a82`
untouched; deployed dyld left at working baseline `10af572e`. LOCAL only.
