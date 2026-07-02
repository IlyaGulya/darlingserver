/* perf#24c2d-fix2 — DCC4 cache format (UNIFIED ARENA, delta-exact layout).
 *
 * DCC2 (region-packed) is DEAD: it packed all __TEXT into one region and all __DATA into another,
 * far apart, so each image's TEXT->DATA distance changed. x86-64 code has RIP-relative displacements
 * baked into instructions (jmp *disp(%rip) stubs, GOTPCREL mov/lea, local __const refs) that a pointer
 * fixup table cannot touch => they broke => the "__text" instruction-fetch SIGSEGV (perf#24c2d-callsite).
 *
 * DCC4 instead lays every image out in ONE arena at a rigid slab, preserving each image's ORIGINAL
 * intra-image segment spacing EXACTLY:
 *     seg.arena_off == image.slab_base + (orig seg.vmaddr - orig __TEXT.vmaddr)
 * so for every image  (arena DATA) - (arena TEXT) == (orig DATA vmaddr) - (orig TEXT vmaddr).
 * A single uniform runtime slide relocates everything, and ALL baked RIP-relative refs stay valid
 * WITHOUT rewriting any code.
 *
 * Permissions differ per segment, so the arena is mapped ONE MAP_PRIVATE region (writable), fixups are
 * applied, then a RUN TABLE re-mprotects contiguous same-permission page runs (RX / RW / RO). Slabs are
 * packed so no page ever holds two permissions (build-time gate). VMA count == number of perm runs.
 *
 * Fixup locations and internal-bind targets are ARENA OFFSETS (not region+offset), so:
 *     rebase:        *(arena + loc_off)              += slide
 *     bind internal: *(arena + loc_off)               = (arena + tgt_off) + slide + addend
 *     bind extern:   *(arena + loc_off)               = <dyld resolver>(sym) + addend
 *     bind extern lazy: resolve-or-sentinel(0), never hard-fail (Darling stub gaps)
 *
 * The reader NEVER lets a DCC image enter applyFixupsToImage / WrappedMachO::forEachFixup.
 */
#ifndef DCC4_FORMAT_H
#define DCC4_FORMAT_H
#include <stdint.h>

#define DCC4_MAGIC   0x44434334u  /* "DCC4" */
#define DCC4_VERSION 4
#define DCC4_PAGE    0x1000        /* perm-run granularity (x86_64 page) */
#define DCC4_ARENA_ALIGN 0x4000
#define DCC4_MAX_SEGS 8
#define DCC4_MAX_RUNS 256          /* perm runs in the run table */

/* fixup kinds (same semantics as DCC2, but loc_off/tgt_off are ARENA offsets) */
#define DCC_FIX_REBASE           0  /* *(arena+loc_off) += slide */
#define DCC_FIX_BIND_INTERNAL    1  /* *(arena+loc_off) = arena + tgt_off + slide + addend */
#define DCC_FIX_BIND_EXTERN      2  /* *(arena+loc_off) = resolver(sym) + addend  (hard-fail if unresolved) */
#define DCC_FIX_BIND_EXTERN_LAZY 3  /* like EXTERN but from a LAZY bind: resolve-or-sentinel(0), no hard-fail */

/* permission codes for runs (match Mach-O initprot bits: R=1,W=2,X=4) */
#define DCC4_PROT_RX 5
#define DCC4_PROT_RW 3
#define DCC4_PROT_RO 1

struct dcc4_run { uint64_t arena_off; uint64_t size; uint32_t prot; uint32_t _pad; };
struct dcc4_seg { char name[16]; uint64_t arena_off; uint64_t vmsize; uint64_t filesize; uint32_t prot; uint32_t _pad; };
struct dcc4_image {
    char     path[256];
    uint8_t  uuid[16];
    uint64_t src_inode, src_mtime, src_size;
    uint64_t image_arena_off;   /* arena offset of this image's __TEXT (== mach_header) */
    uint32_t nsegs, init_index;
    uint32_t fixup_first, fixup_count;
    struct dcc4_seg segs[DCC4_MAX_SEGS];
};
struct dcc4_fixup {
    uint8_t  kind;          /* DCC_FIX_* */
    uint8_t  _pad0[3];
    uint32_t image_index;   /* which image (for invariant/logging) */
    uint64_t loc_off;       /* ARENA offset of the fixup location (must be in a RW run) */
    uint64_t tgt_off;       /* BIND_INTERNAL: ARENA offset of the target */
    int64_t  addend;
    uint32_t extern_sym;    /* BIND_EXTERN[_LAZY]: byte offset into extern strtab (0=none) */
    uint32_t _pad1;
};
struct dcc4_header {
    uint32_t magic, version, arch, image_count;
    uint64_t closure_hash;
    char     install_root[256];
    uint64_t arena_size;        /* total arena virtual span (page-aligned) */
    uint64_t arena_file_off;    /* file offset of the packed arena blob */
    uint32_t run_count;         /* number of perm runs */
    uint32_t _pad_r;
    uint64_t run_off;           /* file offset of dcc4_run[run_count] */
    uint64_t fixup_off;  uint32_t fixup_count;  uint32_t _pad0;
    uint64_t extern_off; uint32_t extern_size;  uint32_t _pad1;
    /* layout: header, image_table[image_count], run_table[run_count], fixups[fixup_count],
     *         extern strtab, then the single arena blob (arena_size bytes). */
};

#endif
