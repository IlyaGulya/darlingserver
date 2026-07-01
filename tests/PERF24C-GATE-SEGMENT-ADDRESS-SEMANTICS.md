# perf#24c-gate — dyld segment address semantics for plane-sliced images

**HARD GATE, no code.** Before building the cache (perf#24c1) or reader (perf#24c2), prove dyld can correctly
locate *every segment* of a logical image whose bytes live in shared plane slices. This is the negotiable
invariant the user set: either `loadedAddress + LC_SEGMENT.vmaddr` resolves to the right slice, **or** the
dyld-cache path has an explicit per-segment translation table that *every consumer* uses. The forbidden state:
mach_header in the TEXT plane but DATA/LINKEDIT elsewhere while dyld assumes one simple slide.

## Finding — dyld uses the SINGLE-SLIDE model everywhere (not a translation table)

Runtime segment addressing in dyld3 goes through **`MachOLoaded::getSlide()`**
(`dyld3/MachOLoaded.cpp:400`):
```
slide = (uintptr_t)this - __TEXT.vmaddr          // 'this' == loadedAddress
segment_i runtime addr = segment_i.vmaddr + slide
```
Every address consumer uses this single TEXT-derived slide:
- **fixups** (`applyFixupsToImage`, Loading.cpp:797): `imageLoadAddress + fixupLocRuntimeOffset`, `slide = loadedAddress->getSlide()`.
- **dladdr** (`findClosestSymbol`) and **findSectionContent** (MachOLoaded.cpp:691): `sectAddr + getSlide()`.
- **intersectsRange** (MachOLoaded.cpp:701): `vmAddr + getSlide()`.

`forEachCacheSegment` (Closure.cpp:426, per-segment `cacheOffset`) **exists but is used only for *mapping* and
*logging*** — it is **not** consulted by the runtime address resolvers above. So there is **no translation
table in the hot path**; option (b) is effectively not implemented for address resolution.

**Consequence:** for any image, every segment MUST sit at `loadedAddress + (segment.vmaddr − TEXT.vmaddr)`.
DATA and LINKEDIT are pinned relative to TEXT by their *vmaddr deltas*.

## This INVALIDATES the perf#24c0 "keep vmaddr unchanged, 3 independent planes" sketch
If TEXT/DATA/LINKEDIT go into three **independent** mappings at unrelated addresses while each image keeps its
original vmaddrs, then `loadedAddress(TEXT) + (DATA.vmaddr − TEXT.vmaddr)` computes a DATA address inside the
TEXT plane — **wrong**. The single-slide model breaks. perf#24c0's audit correctly proved *packability of
bytes* (no text relocs, no DATA_CONST, fixups in DATA) but its "vmaddr UNCHANGED" claim only holds if the
planes preserve each image's inter-segment vmaddr spacing — which naive tight packing destroys.

## How the macOS shared cache actually satisfies single-slide (the correct model)
Darling's tree carries the real dyld3 `shared-cache/CacheBuilder.cpp`. It does **not** keep original vmaddrs —
it **reassigns each dylib's segment vmaddrs** into per-permission cache regions (read-execute / read-write /
read-only) laid out contiguously, and records per-segment `cacheOffset`. Because vmaddrs are rewritten to match
the region layout, a single slide (`cacheBase + textCacheOffset − TEXT.vmaddr_new`) reproduces every segment's
address. The VMA collapse comes from **all images' same-permission segments being contiguous in one region** →
the kernel maps each region as one (few) VMAs, and each image's single slide still resolves correctly.

So the two goals are reconciled by **vmaddr rewriting**, exactly what perf#24c0 said must NOT be needed for
*code* (true — no text bytes change) but IS needed for the *segment vmaddr fields in the load commands*.

## What survives a vmaddr rewrite (bounds the builder's work)
- **rebase/bind opcodes** use `REBASE/BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB` — **segment-index + offset**, not
  absolute vmaddr (MachOAnalyzer.cpp:1234). They resolve against the segment's *runtime* address → **survive
  vmaddr rewrite unchanged.**
- **LINKEDIT consumers** (LC_DYLD_INFO/SYMTAB/DYSYMTAB/FUNCTION_STARTS/DATA_IN_CODE) are **file offsets** →
  rewritten by the linkedit-plane delta (per perf#24c0), independent of vmaddr.
- **What the builder MUST rewrite:** each `LC_SEGMENT_64.{vmaddr, fileoff}` so segments land at region-relative
  positions reproducible by one slide, plus the LINKEDIT file offsets. Code/text bytes untouched.

## GATE VERDICT — proceed, but with a corrected model + a mandatory live smoke
- **Answer to the invariant:** dyld resolves via **single slide off TEXT**, not a translation table. Therefore
  the cache MUST place each image's segments at their vmaddr-delta positions from a single per-image base — i.e.
  the **macOS "contiguous per-permission regions with rewritten vmaddrs" model**, NOT three independent planes
  with original vmaddrs.
- **inDyldCache() path does NOT already do multi-mapping-per-image** in a way our reader can borrow directly
  for arbitrary plane layouts — it assumes the cache is one region and images are pre-fixed-up (fixups skipped,
  Loading.cpp:331). So per the user's rule: **a tiny 2-dylib live reader smoke is REQUIRED before the full
  closure** — static proof is insufficient because the single-slide interaction with our region layout must be
  executed.

### Corrected design handed to perf#24c1/c2
- **Plane model:** not 3 arbitrary planes — use **per-permission contiguous regions** (RX region = all TEXT,
  RW region = all DATA, RO region = all LINKEDIT), with each image's segment **vmaddrs rewritten** so that
  `image_base + (seg.vmaddr − text.vmaddr)` lands in the right region. `image_base` = region-relative TEXT
  address; DATA/LINKEDIT deltas must equal (their region address − TEXT region address). This constrains the
  layout: an image's (DATA.vmaddr − TEXT.vmaddr) must equal (dataRegionBase + itsDataOff) − (textRegionBase +
  itsTextOff). The builder solves this by assigning vmaddrs from the region bases (as macOS does).
- **c1 builder:** reassign vmaddrs from region bases; emit RX/RW/RO regions + image table (path, uuid/inode/
  mtime/size, rewritten vmaddr/fileoff per seg, init index). Deterministic, temp+atomic-rename, reject stale,
  no partial cache, env-gated.
- **c2 reader:** map the 3 regions (RX+RO MAP_SHARED, RW MAP_PRIVATE/COW), set each image's loadedAddress into
  the RX region at its assigned base; **skip per-image fixups only if the builder pre-applied rebase** (else
  apply rebase to the private RW copy per process). **2-DYLIB LIVE SMOKE FIRST** proving getSlide() resolves
  TEXT+DATA+LINKEDIT correctly (call a function, read a DATA global, dladdr a symbol) before the full closure.

### Explicitly NOT done / NOT touched
No cache built, no dyld patched (submodule `M` = pre-existing pointer diff), no init laziness, no prune, no
reuse/vfork/ring/psynch/fd-band, no LLVM. prod dserver `835946f9` + mldr `f0cd2a82` untouched.

## Method
Source read of dyld3: `MachOLoaded::getSlide` (:400), `findSectionContent`/`intersectsRange` (:691/:701),
`applyFixupsToImage` (:791), `Image::cacheOffset`/`forEachCacheSegment` (Closure.cpp:417/426), mapAndFixup
cache path (Loading.cpp:302-344), rebase opcode addressing (MachOAnalyzer.cpp:1234), shared-cache CacheBuilder
region model. Reuses perf#24c0 + perf#24b.
