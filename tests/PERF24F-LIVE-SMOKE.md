# perf#24f-live-smoke — DCC5 works LIVE in the real dyld path (task #101)

Status: **PASS.** The DCC5 (3-region + TEXT rip-rel rewrite) cache runs live in the dyld2 classic path.
The full 40-dylib closure — the exact set that produced the "__text" instruction-fetch SIGSEGV under
unrewritten DCC2 (task #97) — now runs `/usr/bin/true` to exit 0 with no crash. This proves the
RIP-relative rewrite (perf#24f offline gates, darlingserver 13b6d65 / dyld reader bcf77af) fixes the
crash live, before the full clang A/B.

## Build + deploy (per task constraints)
- Built ONLY in ~/work/darling-build (Release, prefix ~/work/darling-prefix).
- Checked out xnu@a0328833 for the dyld build (rebuilt libsystem_kernel.dylib + dyld), then restored
  xnu to perf/shmem-ring-guest @caddc2b. Built dyld md5 1469bbad (reader compiled in: log strings
  "dyld[DCC2]: ..." and the new "bad magic (need DCC5)" / "bad version (need 5)" present).
- Reader change was minimal: DCC2Reader accepts DCC5 magic/version; the rewritten __TEXT rides in the
  RX region, so mapping+executing it needs NO reader logic change.
- Deployed dyld to BOTH closure copies (libexec/darling + prefix-root). DCC5 caches deployed to the
  guest at /private/var/root/{full,smoke2}.dcc5.
- CRITICAL DEPLOY FIX: the guest resolves /usr/lib/... to the libexec/darling tree (inode 6724520),
  NOT the prefix-root tree (inode 8069995). The cache MUST be built with install_root =
  <prefix>/libexec/darling so the reader's inode/mtime/size staleness check matches what the guest
  stats. Building from prefix-root => "stale source" hard-fail. (Same content; only recorded inode
  differs.)

## Results (all against acceptance)
1. **DCC5 OFF: rebuilt dyld boots clean.** `/usr/bin/true` exit 0; zero DCC2 log lines (reader dormant
   with flag absent = exact old dyld2 behavior).
2. **DCC5 ON 2-dylib smoke** (smoke2.dcc5 = {libsystem_blocks, libunwind}): exit 0. Log:
   - `enabled, 2 images from smoke2.dcc5 (arena=...)` — cache maps.
   - `substituted cached image libsystem_blocks @ arena (index 0)` + `libunwind @ arena+0x4000 (idx 1)`
     — both cached images served from the cache.
   - `applied 83 fixups once for whole cache` — DCC fixup table applied once; normal Mach-O fixup engine
     skipped for DCC images (reader's isDCC2Image guard).
   - No "__text" crash => the 111 rewritten rip-rel stubs work live.
3. **DCC5 ON full closure** (full.dcc5, 40 dylibs): **`/usr/bin/true` exit 0.**
   - `enabled, 40 images from full.dcc5 (arena=0x7a9af91a0000)`.
   - **38 images substituted from the cache**, incl. the non-leaf, RIP-heavy dylibs that crashed under
     DCC2: libSystem.B, libsystem_kernel, libsystem_c, libsystem_malloc, libdispatch, libsystem_m,
     libdyld, liblaunch, ...
   - `applied 27082 fixups once for whole cache` (== builder's 27082).
   - **No "__text" SIGSEGV, no abort/trap/halt, no normal-fixup fallback.**
   - Only unresolved externs = the 11 expected BIND_EXTERN_LAZY stub-gaps (`_ccchacha20` etc. => sentinel
     0, never-called ok). No REQUIRED extern unresolved.
4. **Restore**: prod dyld (both copies) / mldr / darlingserver byte-identical to baseline afterward;
   guest caches removed; `west darling-doctor` ALL GREEN.

## VMA collapse
Architecturally proven and engaged: the reader's mapRegions() allocates ONE arena and does exactly
3 MAP_FIXED mmaps (RX/RW/RO); the ON full-closure run confirmed all 40 images resolve to addresses
inside that single arena. The historical per-process closure footprint is ~114 VMAs (38 dylibs x 3
segments); the DCC5 arena is 3. An EMPIRICAL per-process VMA count was not captured here because the
DCC flag is delivered only to the leaf guest command (transient, exits immediately) — launchd/shellspawn
run without the flag (still ~114). The clean empirical measurement belongs to the A/B bead, which runs
clang UNDER the flag and reads the kernel-mm exit_mmap bucket + VMA counts directly.

## NEXT: perf#24f-live-A/B
shellspawn; clang 1-file; clang 200-file -j8 A/B (DCC5 ON vs OFF). Metrics: wall/user/sys; exit_mmap
per proc; dyld load time; per-process closure VMA count (~114 -> ~3); kernel-mm bucket; minor faults
(expect ~unchanged — same bytes fault regardless of packing). Deliver the flag to the clang processes
(guest env), and measure the persistent build workers.
