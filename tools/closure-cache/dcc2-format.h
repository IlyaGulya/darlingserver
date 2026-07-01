/* perf#24c2b — DCC2 cache format (region-packed + cache-native fixup table).
 *
 * Extends DCC1 (perf#24c1): same 3 per-permission regions (RX=all __TEXT, RW=all __DATA,
 * RO=all __LINKEDIT) with rewritten LC_SEGMENT_64.vmaddr in the packed header (perf#24c2a fix),
 * PLUS a precomputed cache-native fixup table so dyld can skip the normal Mach-O fixup engine
 * (which is known-wrong for region-packed layout — perf#24c2a red line).
 *
 * All fixup locations and internal-bind targets are CACHE-RELATIVE (region id + offset), so a
 * single uniform runtime slide relocates everything:
 *   rebase:        *(RW_base + loc_off)              += slide
 *   bind internal: *(RW_base + loc_off)               = (region[tgt_region].vm_base + tgt_off) + slide
 *   bind extern:   *(RW_base + loc_off)               = <dyld symbol resolver>(extern_sym) + addend
 *
 * The reader NEVER lets a DCC image enter applyFixupsToImage/WrappedMachO::forEachFixup.
 */
#ifndef DCC2_FORMAT_H
#define DCC2_FORMAT_H
#include <stdint.h>

#define DCC2_MAGIC   0x44434332u  /* "DCC2" */
#define DCC2_VERSION 2
#define DCC2_REGION_ALIGN 0x4000
#define DCC2_MAX_SEGS 8

/* fixup kinds */
#define DCC_FIX_REBASE        0   /* *loc += slide (target is same image) */
#define DCC_FIX_BIND_INTERNAL 1   /* *loc = region[tgt_region].vm_base + tgt_off + slide */
#define DCC_FIX_BIND_EXTERN   2   /* *loc = resolver(extern_sym) + addend  (runtime symbol lookup) */

/* pointer type for a fixup location (all are 8-byte pointers in x86_64 __DATA here) */
struct dcc_region { uint64_t file_off; uint64_t size; uint64_t vm_base; uint32_t prot; uint32_t _pad; };
struct dcc_seg    { char name[16]; uint64_t vmaddr; uint64_t vmsize; uint64_t region_off; uint64_t filesize; uint32_t region_idx; uint32_t prot; };
struct dcc_image {
    char     path[256];
    uint8_t  uuid[16];
    uint64_t src_inode, src_mtime, src_size;
    uint64_t image_vmbase;   /* rewritten TEXT vmaddr (region-relative) */
    uint32_t nsegs, init_index;
    uint32_t fixup_first, fixup_count;   /* range into the global fixup array */
    struct dcc_seg segs[DCC2_MAX_SEGS];
};
struct dcc_fixup {
    uint8_t  kind;          /* DCC_FIX_* */
    uint8_t  loc_region;    /* region holding the fixup location (normally RW=1) */
    uint8_t  tgt_region;    /* for BIND_INTERNAL: region of the target */
    uint8_t  _pad;
    uint32_t image_index;   /* which image this fixup belongs to (for invariant checks/logging) */
    uint64_t loc_off;       /* offset of the fixup location within loc_region */
    uint64_t tgt_off;       /* BIND_INTERNAL: target offset within tgt_region */
    int64_t  addend;        /* rebase/bind addend */
    uint32_t extern_sym;    /* BIND_EXTERN: byte offset into extern strtab (0 = none) */
    uint32_t _pad2;
};
struct dcc_header {
    uint32_t magic, version, arch, image_count;
    uint64_t closure_hash;
    char     install_root[256];
    struct dcc_region regions[3];   /* 0=RX 1=RW 2=RO */
    uint64_t fixup_off;   uint32_t fixup_count;  uint32_t _pad0;   /* dcc_fixup[fixup_count] */
    uint64_t extern_off;  uint32_t extern_size;  uint32_t _pad1;   /* extern symbol strtab */
    /* layout: header, image_table[image_count], fixups[fixup_count], extern strtab, then 3 region blobs */
};

#endif
