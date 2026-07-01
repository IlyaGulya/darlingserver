# perf#24c2a — DCC1 reader RED LINE + DCC2 rescope + fixup/bind class audit

**Outcome:** the DCC1 flag-gated live-reader attempt was **stopped at a RED LINE** (dyld's normal fixup engine
assumes contiguous per-image Mach-O layout, which the region-packed cache breaks). Rescoped to **DCC2**: a
region-packed cache that carries a **cache-native precomputed fixup table**, with a dyld DCC path that
**skips normal Mach-O fixups** for cached images (the macOS shared-cache model). Along the way a real
**builder bug was found and fixed**, and a full **fixup/bind class audit** was run to size DCC2.

No dyld code was written and nothing was deployed. prod dserver `835946f9` + mldr `f0cd2a82` + deployed dyld
`10af572e` all UNTOUCHED. LOCAL only.

---

## 1. Builder bug found + fixed (only a reader-proxy could catch it)

The perf#24c1 builder rewrote each image's segment vmaddrs **only in the `dcc_seg` table**, NOT in the packed
**mach-o header** (`LC_SEGMENT_64.vmaddr`). But dyld reads segment vmaddrs from the **header** — via
`MachOLoaded::getSlide()` (`loadedAddress − __TEXT.vmaddr`) and `preferredLoadAddress()` — never from our
table. A standalone **reader-proxy harness** (`slide-harness.c`) that maps the 3 regions preserving fixed
relative vm-base spacing and then reads seg vmaddrs from the header **exactly like dyld**:
- **FAILED** on the table-only cache: DATA/LINKEDIT resolved to the wrong address, and the second image's TEXT
  slide was wrong (header said `__TEXT.vmaddr=0` while it was placed at `image_vmbase=0x8000`).
- **PASSED** after adding a header-rewrite pass to the builder (patch `LC_SEGMENT_64.vmaddr` in the RX-region
  header copy after the segment byte copy).

→ Fix applied to `tools/closure-cache/closure-cache-builder.c`. (`ccvalidate`'s byte-identity assert now needs
an update: header vmaddr bytes intentionally differ from the source slice; DCC2 changes the validator anyway.)

## 2. The RED LINE — region model is incompatible with dyld's normal fixup engine

- Darling **does** take the dyld3 closure path at launch:
  `buildLaunchClosure → launchWithClosure → mapAndFixupAllImages → mapImage`. Confirmed **live** via
  `DYLD_PRINT_SEGMENTS=1 /usr/bin/true`: `dyld: Mapping /usr/lib/…` + 3 segments/dylib (`r-x`/`rw-`/`r--`),
  mapped from disk (no prebuilt shared cache → `inDyldCache()` is false → `mapImage`). Injection point is
  correct and live.
- BUT `applyFixupsToImage` (Loading.cpp:809) → `WrappedMachO::forEachFixup` (MachOAnalyzerSet.cpp:136) assumes
  each image is **one contiguous vmaddr span `[TEXT|DATA|LINKEDIT]`** with `vmaddr-delta == fileoff`:
  - `fixupOffset = fixupLoc − mh` (image-base byte offset),
  - rebase `targetOffset` is relative to `preferredLoadAddress()` (original `__TEXT.vmaddr`),
  - chained-fixup starts use `dyld_chained_starts_in_segment.segment_offset` from the image base.
  - Measured: every closure dylib has `DATA−TEXT vmaddr delta == DATA fileoff` (libSystem.B `0x8000`,
    libsystem_c `0xc5000`, libdispatch `0x84000`, …) — i.e. the normal contiguous Mach-O layout.
- The region model **scatters** an image's 3 segments into 3 distant per-permission regions. The single-slide
  `getSlide()` is satisfied (§1), but the **contiguity** assumption the fixup walker relies on is not → rebase
  and bind locations/targets compute into the wrong place.
- macOS shared cache avoids this **only** because `CacheBuilder` **pre-applies all fixups at build time** and
  marks images `inDyldCache()` so dyld **skips** fixups entirely (Loading.cpp:331 `continue`). Darling's
  `inDyldCache` skip requires the real `DyldSharedCache` struct, not our DCC.

Per the c2a spec RED LINE (*"dyld has too many hidden assumptions about original file layout"*), we stopped
rather than papering over it with full-closure hacks.

**Decision (user): Option A → DCC2.** Keep region packing (the VMA-collapse goal is non-negotiable), make the
cache carry a **precomputed / cache-native fixup table**, and add a dyld DCC path that **skips normal Mach-O
fixups** for cached images and applies the cache-native table instead. Do NOT fall back to per-image contiguous
blocks (that abandons max VMA collapse), and do NOT emit a real `DyldSharedCache`.

## 3. Fixup / bind class audit (sizes DCC2's precomputed table)

Tool: `fixup-audit.c` — walks each dylib's LINKEDIT, classifies encoding + rebase/bind counts + bind target
classes. Run over the 2-dylib smoke set and the full 40-dylib closure.

### Full 40-dylib closure
- **Encoding: 100% opcode** (`LC_DYLD_INFO_ONLY`). **Zero chained fixups** across the entire closure → DCC2's
  precompute walks opcode streams only; no chain-walk machinery needed.
- **Totals: 23,407 rebases + 1,395 binds.**
- **Bind classes:** 1,236 two-level `ord=` binds · 154 `self` (re-export, mostly libSystem.B) · **5 `flat`**
  (libsystem_blocks ×2, libunwind ×3) · 0 weak · 0 main-exe · 0 other.
- **Every `ord=` bind target dylib is inside the closure set** (set difference empty). → **all two-level binds
  are build-time resolvable** when the whole closure is cached together. The builder can precompute each bind
  as `(target image index, offset within cache)`; the reader adds the uniform slide.
- Only edge cases: the **5 flat-namespace binds** (search-all-images at runtime) and `self` re-exports. Flat
  targets are expected to resolve inside the closure but must be handled explicitly (resolve at build if the
  defining image is in the set; else leave for runtime).

### 2-dylib smoke set selection
- `{libmacho, libsystem_blocks}` is **NOT self-contained**: libmacho binds to libsystem_kernel/libdyld/
  libsystem_c/libsystem_malloc (16 ord binds) — outside the pair.
- **Self-contained candidates (no cross-image `ord` binds):** `libSystem.B` (153 self), `libsystem_blocks`
  (2 flat), `libunwind` (3 flat).
- **Chosen smoke pair: `{libsystem_blocks, libunwind}`** — only self/flat binds, so a 2-dylib DCC2 cache can
  precompute/resolve everything without pulling in the rest of the closure. (Flat binds still need their
  defining symbol present at runtime; few and directly testable.)

## 4. DCC2 format requirements (derived from the audit)

The builder must, per cached image:
1. Region-pack as today (RX/RW/RO), rewriting `LC_SEGMENT_64.vmaddr` in the header (§1 fix) so `getSlide()`
   still works for any code that reads the header (dladdr, symbol lookup, etc.).
2. **Precompute a fixup table** by walking the opcode rebase/bind streams:
   - **rebase** → record the fixup location as a **cache-relative offset** (which region + offset) so the
     reader applies `*loc += slide` at one uniform slide. (Rebase target = its own image; slide-add only.)
   - **bind (ord)** → resolve the symbol against the target image (in-closure) at build time → record
     `(fixup cache-offset, target cache-offset)`; reader writes `target + slide`.
   - **bind (self)** → resolve within the same image at build.
   - **bind (flat)** → resolve against the closure set at build if the defining image is present; otherwise
     mark as runtime-resolved (must not silently drop).
3. Emit the table in cache-relative terms (region idx + offset), never absolute, so one slide relocates all.

The reader (DCC2 dyld path):
- Map the 3 regions preserving fixed relative spacing (validated in §1), set each image `loadedAddress` into
  RX at `image_vmbase + slide`, `setState(fixedUp)` and **skip `applyFixupsToImage`** for cached images
  (mirror Loading.cpp:331), then apply the cache-native fixup table against the single uniform slide on the
  private (COW) RW region.
- Flag-gated (`DARLING_DYLD_DCC=1` + `DARLING_DYLD_DCC_PATH`); flag absent = byte-identical old behavior;
  flag present + invalid/stale = clean fallback or hard fail, no half-loaded state.

## Method / artifacts
`$CLAUDE_JOB_DIR/tmp`: `slide-harness.c` (reader-proxy, reads header like dyld — proved the builder bug),
`fixup-audit.c` (+ `audit-full.txt`), `deltas.c` (inter-segment delta measurement), `check_hdr.c` (header
vmaddr verification). Builder fix in `tools/closure-cache/closure-cache-builder.c`. Source reads: Loading.cpp
(mapImage 420, mapAndFixupAllImages 256, inDyldCache skip 302-344/331, applyFixupsToImage 791-899),
MachOAnalyzerSet.cpp:136 (forEachFixup), ClosureBuilder.cpp:1200-1249 (fixup offset/target encoding),
dyld2.cpp:6058/7013-7067 (launchWithClosure / buildLaunchClosure). Reuses perf#24c1/c-gate/c0/24b.
