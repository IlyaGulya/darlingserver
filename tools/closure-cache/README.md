# Darling system-closure cache (perf#24c1 builder)

Standalone host tool that packs the libSystem.B + `/usr/lib/system/libsystem_*` closure (40 dylibs) into a
single cache file with 3 per-permission regions, to collapse the per-process dylib VMA count (~120 → 3) that
drives the `exit_mmap` teardown bucket (perf#22a/24a/24b). **Builder only — no dyld behavior change.** The
flag-gated dyld reader is perf#24c2.

## Build & run
```
gcc -O2 -o ccbuilder closure-cache-builder.c
./ccbuilder <install_root> closure-list.txt system-closure.cache
gcc -O2 -o ccvalidate closure-cache-validate.c
./ccvalidate system-closure.cache      # exit 0 = structure + single-slide + byte-identity OK
```
`install_root` = the dir under which `/usr/lib/...` resolves (e.g. the Darling install tree
`.../darling-prefix/libexec/darling`). Sources are FAT (x86_64 + i386); the builder extracts the x86_64 slice.

## Format (`DCC1`, version 1)
```
header { magic, version, arch, image_count, closure_hash(FNV over path+inode+mtime+size),
         install_root, regions[3] }               // region = {file_off, size, vm_base, prot}
image_table [image_count] { path, uuid, src_inode/mtime/size, image_vmbase(=rewritten TEXT vmaddr),
                            nsegs, init_index, segs[] { name, vmaddr, vmsize, region_off, filesize,
                                                        region_idx, prot } }
region blobs: RX (all __TEXT, prot r-x), RW (all __DATA, prot rw-), RO (all __LINKEDIT, prot r--)
```
- **Deterministic**: closure list sorted; first-fit region packing → byte-identical rebuilds.
- **Atomic**: writes `<out>.tmp` then `rename()`.
- **All-or-nothing**: any fat/Mach-O parse error or invariant violation aborts with no output.
- **Staleness**: per-image `inode/mtime/size` recorded; the reader must reject + fall back if any source
  changed. `closure_hash` covers the whole set.

## The single-slide constraint (from perf#24c-gate — the reader MUST honor this)
dyld3 resolves every segment address via `getSlide() = loadedAddress − rewrittenTEXT.vmaddr` (one slide off
TEXT); it does **not** consult a per-segment translation table at runtime. So the builder **rewrites each
dylib's segment vmaddrs** into region-relative absolute positions, and lays the 3 regions at **fixed relative
vm bases**: `RX.vm_base = 0`, `RW.vm_base = roundup(RX.size)`, `RO.vm_base = roundup(RX+RW)`. Then a **single
uniform slide** applied to all three regions reproduces every segment address:
```
region i mapped at (region[i].vm_base + S)  for one S
image TEXT runtime = image_vmbase + S
seg runtime        = seg.vmaddr   + S       // seg.vmaddr already region-relative-absolute
```
**Reader requirement (perf#24c2):** map the 3 regions so their runtime bases keep the same relative spacing
(`RW_map − RX_map == RW.vm_base − RX.vm_base`, likewise RO) — e.g. one `mmap` of the whole span then
`mmap(MAP_FIXED)` each region, or reserve a contiguous arena. If the relative spacing is preserved, one slide
is correct and `getSlide()` works unmodified. The builder's `ccvalidate` asserts this layout
(`uniform-slide region layout: OK`).

### What survives the vmaddr rewrite (why this is safe)
- **rebase/bind** opcodes use `SET_SEGMENT_AND_OFFSET` (segment-index + offset), not absolute vmaddr → survive.
- **LINKEDIT consumers** (LC_DYLD_INFO/SYMTAB/DYSYMTAB/FUNCTION_STARTS/DATA_IN_CODE) are file offsets → the
  reader/loader reads them at the RO region offset; the builder keeps LINKEDIT contiguous per image so a single
  linkedit base per image works. **No code/text bytes are modified** (closure has 0 text relocs — perf#24c0).
- **DATA** region must be mapped `MAP_PRIVATE`/COW (per-process rebased pointers, TLV, malloc state).

## Validation (ccvalidate, this cache)
40 images / 120 segs → 3 regions; region-layout OK, seg-in-region OK, byte-identical OK, deterministic
(md5-stable), atomic (no leftover .tmp), staleness fields present. VMA projection 120 → 3.
