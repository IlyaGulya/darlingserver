# perf#24c0 — closure packability feasibility gate

**FEASIBILITY GATE, no cache built.** Rescoped from perf#24c (implementation) — the minimal-prototype framing
understated the work: a true VMA collapse needs a packed cache file + a dyld reader. This bead answers *whether
that is feasible* via a Mach-O relocation/layout audit of the libSystem closure + a static toy validator, and
issues one decision (A fund builder / B pivot to init laziness / C STOP). No code changed; prod dserver
`835946f9` + mldr `f0cd2a82` untouched. Closure files audited from the install tree
`darling-prefix/libexec/darling/usr/lib{,/system}` (inode-verified identical to what the guest maps).

## Why a per-dylib shortcut is impossible (mechanism recap)
dyld3 `Loader::mapImage` (`src/external/dyld/dyld3/Loading.cpp:420`) maps each dylib as `vm_allocate(totalVMSize)`
+ N × `mmap(MAP_FIXED|MAP_PRIVATE, fd)` — one per segment. `/proc/maps` confirms **exactly 3 VMAs/dylib**
(`r-xp` TEXT / `rw-p` DATA / `r--p` LINKEDIT), **40 dylibs = 120 dylib VMAs**. You cannot go below 3 per dylib:
the three segments have **distinct permissions** (kernel never coalesces different-perm VMAs), they come from
**different files** (never coalesce across dylibs), and DATA has a vm-gap (`vmsize > filesize`, bss) so a single
whole-file mmap misplaces LINKEDIT. The only way to collapse is a **packed cache file**: concatenate all
closure TEXT into one plane, all DATA into another, all LINKEDIT into a third, so **one mmap covers each plane**.

## 1. Relocation / layout audit (libSystem.B + 25 libsystem_*/closure dylibs)

### Segment linearity (vmaddr vs fileoff)
| dylib | TEXT | DATA | LINKEDIT |
|-------|------|------|----------|
| libSystem.B | va==fo (linear) | va==fo | va==fo (contiguous) |
| libsystem_kernel | va==fo | va==fo | **va≠fo** (shifted by DATA bss gap) |
| libsystem_c / malloc / dispatch | va==fo | va==fo | **va≠fo** (same reason) |

**TEXT and DATA are always linear** (`vmaddr == fileoff`). **LINKEDIT is offset by `DATA.vmsize −
DATA.filesize`** (the bss zero-fill gap) — but each segment is still independently, linearly mappable from its
own `(fileoff, filesize)` → `(vmaddr, vmsize)`, which is exactly what per-segment mmap already does. Packing
relocates each segment's *file bytes* into a plane at a new file offset; **vmaddr/vmsize stay unchanged**.

### Relocations / fixups (the "no text rewriting" question)
Across **all 26 audited dylibs**: **`__TEXT` relocation sections = 0.** Code is position-independent
(RIP-relative); there are **no text relocs / no RIP-relative hazards** requiring code patching. All fixups are
**rebase (slide-relative) + bind (symbol)** opcodes in `LC_DYLD_INFO_ONLY`, targeting **DATA** — applied by
dyld at load exactly as today (rebase counts vary: dispatch 5699, kernel 1240; all land in DATA).

### __DATA_CONST (the re-split hazard)
**No closure dylib has `__DATA_CONST`.** So the `applyFixupsToImage` `hasReadOnlyData()` re-mprotect
(`Loading.cpp:991`) — which would re-split a coalesced region back into per-segment VMAs after fixup — **never
fires for this closure.** (This old Apple LLVM-9 toolchain predates DATA_CONST; a real advantage here.) Packed
planes therefore *stay* 3 VMAs; they don't re-split.

### LINKEDIT consumers (what the builder must fix up)
`LC_DYLD_INFO_ONLY` (rebase/bind/weak_bind/lazy_bind/export offsets), `LC_SYMTAB` (symoff/stroff),
`LC_DYSYMTAB` (indirectsymoff…), `LC_FUNCTION_STARTS`, `LC_DATA_IN_CODE` — all reference LINKEDIT by **absolute
file offset**. When LINKEDIT bytes move to the packed plane, **every one of these must be rewritten** by the
delta `(new_linkedit_off − old_linkedit_off)`. Plus each `LC_SEGMENT_64.fileoff`. This is a **header-only
rewrite in each dylib's preserved Mach-O header — no code/text bytes touched.**

### Unwind / eh_frame
`__unwind_info` / `__gcc_except_tab` / `__eh_frame` all live in **`__TEXT`** (RO, shareable). The unwinder finds
them via each dylib's mach header (preserved), so unwinding works unchanged in the packed layout.

## 2. Answer — YES, packable into separate planes without text rewriting
**Can we pack TEXT/DATA/LINKEDIT into separate planes without text rewriting? — YES.** No text relocs, no
DATA_CONST, PIC code, all dynamic fixups in DATA. The builder must apply only **header-level offset fixups**
(per the toy transform below):
1. `LC_SEGMENT_64.fileoff` → plane base + packed offset (TEXT→text plane, DATA→data plane, LINKEDIT→link plane).
2. `LC_DYLD_INFO_ONLY` {rebase,bind,weak_bind,lazy_bind,export}_off += linkedit delta.
3. `LC_SYMTAB` {symoff,stroff} += linkedit delta.
4. `LC_DYSYMTAB` {indirectsymoff,…} += linkedit delta.
5. `LC_FUNCTION_STARTS` / `LC_DATA_IN_CODE` dataoff += linkedit delta.
6. `LC_CODE_SIGNATURE` dropped (Darling doesn't enforce codesign) or offset-fixed.
**vmaddr/vmsize UNCHANGED** ⇒ rebase/bind targets, dladdr, dlsym all resolve identically ⇒ identity preserved.
Mapping: **TEXT + LINKEDIT planes `MAP_SHARED` RO** (faulted once, shared cross-process); **DATA plane
`MAP_PRIVATE`/COW** (per-process writable, holds rebased pointers/TLV/malloc state).

## 3. Toy validator (static) — PASS
`p24c0-validate.sh` (static invariant check over the closure) + `p24c0-pack.py` (2-dylib packing transform):
- **26/26 dylibs, 52/52 checks pass**, 0 text-relocs, 0 DATA_CONST, unwind-in-TEXT, all DYLIB. → the closure
  is uniformly packable.
- Packing transform on {libSystem.B, libsystem_kernel} produces valid per-plane offsets; **vmaddr/vmsize
  unchanged** (proves logical **image identity + dlsym/dladdr** — they key off `loadedAddress()` + the
  preserved Mach-O header/segment layout, which the transform doesn't alter). **Init order** is driven by the
  closure dependency graph (unchanged by how bytes are mapped). **DATA COW** modelled as `MAP_PRIVATE`. VMA
  count 6 → 3 planes; at 40 dylibs, **120 → 3**.
- A *live* 2-dylib cache reader was deliberately NOT built: it requires patching + rebuilding dyld (teaching a
  new plane-mapping path), which is precisely the perf#24c1 work this gate authorizes — building it here would
  pre-empt the decision. The static proof establishes correctness feasibility; the live reader is the funded
  next step.

## 4. DECISION — **A: full packed-cache builder is FEASIBLE → fund perf#24c1**
Every packability hazard is absent: no text relocs (no code rewriting), no DATA_CONST (no post-fixup re-split),
PIC code, all fixups in DATA, unwind in TEXT, LINKEDIT consumers are mechanical header-offset rewrites. The
transform preserves vmaddr/vmsize ⇒ full Mach-O identity. Projected win (perf#24b upper-bound): **120 → ~3–6
VMAs, exit_mmap teardown ~480→~162 µs/proc (~127 ms build-wide), load ~19×.** Not B (no text rewriting needed;
the closure is clean) — init laziness stays the *separate* perf#24d lever (orthogonal, doesn't need this).
Not C (the VMA reduction is large and layout-preserving).

### Scope handed to perf#24c1 (build) + perf#24c2 (dyld reader)
- **perf#24c1 — cache builder** (offline/first-boot tool): read the closure, emit 3 planes + an image table
  (per-dylib: leafName/path, plane offsets, vmaddr base, init pointer, UUID), applying the header fixup set
  above. Static-validate the emitted cache re-parses per-dylib.
- **perf#24c2 — dyld reader + A/B**: flag-gated path in `mapAndFixupAllImages` that maps the 3 planes once and
  sets each closure image's `loadedAddress` into its plane slice (mirroring the existing `inDyldCache()` fast
  path at `Loading.cpp:303`, which already avoids per-image mmap), preserving init order; then A/B measure VMA
  count / mmap load / exit_mmap teardown ON vs OFF on the 200-file build, and validate dladdr/dlsym/init/fork.
- **Known constraints to carry**: DATA MAP_PRIVATE COW; per-dylib vmaddr slots still need reservation (the
  reader points into planes, doesn't per-dylib vm_allocate); rebase must be applied to the private DATA plane
  per process (or precomputed at a fixed slide); TLV descriptors (`MH_HAS_TLV_DESCRIPTORS` on libSystem.B /
  libsystem_c) live in DATA and are handled by existing TLV setup.

### Explicitly NOT done / NOT touched
No cache built, no dyld patched (submodule `M` in git status is a pre-existing pointer diff, working tree
clean), no in-arena partial variant, no dylibs pruned, no init laziness, no reuse/vfork/ring/LLVM/bind. prod
dserver `835946f9` + mldr `f0cd2a82` untouched; sysctls unchanged.

## Method / artifacts
Host `llvm-objdump --macho` (`--private-headers` / `--reloc` / `--rebase` / `--bind` / `--section-headers`) on
the install-tree closure (inode-verified == guest mappings). `p24c0-validate.sh` (static invariants),
`p24c0-pack.py` (2-dylib packing transform + builder fixup set). Reuses perf#24b + perf#24a.
