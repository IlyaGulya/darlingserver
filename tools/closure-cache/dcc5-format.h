/* perf#24f — DCC5 cache format (region-packed + cache-native fixup table + TEXT rip-rel REWRITE).
 *
 * IDENTICAL binary layout to DCC2 (3 per-permission regions RX/RW/RO + fixup table + extern strtab),
 * with ONE added guarantee the DCC2 builder did NOT provide:
 *
 *   The packed __TEXT bytes have their x86-64 RIP-relative displacements REWRITTEN so that every
 *   TEXT->DATA reference (jmp/call *disp(%rip) stubs, GOTPCREL mov/lea, local __const/__data refs)
 *   still points at the correct target after the 3-region relayout moved __TEXT into RX and __DATA
 *   far away into RW.
 *
 * WHY: DCC2 collapsed VMAs (3 regions) but a POINTER-fixup table cannot touch instruction immediates,
 * so every baked RIP-relative TEXT->DATA disp32 was left stale => the "__text" instruction-fetch
 * SIGSEGV (root cause task #97). DCC4 preserved the disps by keeping each image's TEXT<->DATA delta
 * (delta-exact) but that forces perms to cycle every image => 120 VMAs = zero collapse (task #98).
 * perf#24e proved the rewrite is bounded+safe: of 42002 rip-rel sites in closure __text, only the
 * 23182 that cross to the moved RW region need rewriting (all fit int32); 18636 same-TEXT sites move
 * rigidly with __TEXT and are LEFT UNCHANGED; jump tables are TEXT-internal (same-TEXT) => untouched.
 *
 * The rewrite is APPLIED AT BUILD TIME to the packed RX bytes. Therefore the format/magic can stay
 * DCC2-compatible for the reader (same region model, same fixup semantics); DCC5 exists as a distinct
 * magic only so a reader/validator can assert "this cache has had its TEXT rewritten" and refuse a
 * stale DCC2 (which would crash) — a stale DCC2 fed to a DCC5-expecting path is a RED arm.
 *
 * Fixup table semantics unchanged from DCC2 (all cache-relative region+offset, one uniform slide):
 *   rebase:            *(RW_base + loc_off)  += slide
 *   bind internal:     *(RW_base + loc_off)   = region[tgt_region].vm_base + tgt_off + slide + addend
 *   bind extern:       *(RW_base + loc_off)   = <dyld resolver>(extern_sym) + addend  (hard-fail)
 *   bind extern lazy:  resolve-or-sentinel(0), never hard-fail (Darling stub gaps)
 *
 * The reader NEVER lets a DCC image enter applyFixupsToImage/WrappedMachO::forEachFixup.
 */
#ifndef DCC5_FORMAT_H
#define DCC5_FORMAT_H
#include <stdint.h>

#define DCC5_MAGIC   0x44434335u  /* "DCC5" */
#define DCC5_VERSION 5
#define DCC5_REGION_ALIGN 0x4000
#define DCC5_MAX_SEGS 8

/* fixup kinds (identical to DCC2) */
#define DCC_FIX_REBASE        0   /* *loc += slide (target is same image) */
#define DCC_FIX_BIND_INTERNAL 1   /* *loc = region[tgt_region].vm_base + tgt_off + slide */
#define DCC_FIX_BIND_EXTERN   2   /* *loc = resolver(extern_sym) + addend  (runtime symbol lookup; hard-fail if unresolved) */
#define DCC_FIX_BIND_EXTERN_LAZY 3 /* like EXTERN but from a LAZY bind: resolve-or-sentinel(0), no hard-fail */

struct dcc_region { uint64_t file_off; uint64_t size; uint64_t vm_base; uint32_t prot; uint32_t _pad; };
struct dcc_seg    { char name[16]; uint64_t vmaddr; uint64_t vmsize; uint64_t region_off; uint64_t filesize; uint32_t region_idx; uint32_t prot; };
struct dcc_image {
    char     path[256];
    uint8_t  uuid[16];
    uint64_t src_inode, src_mtime, src_size;
    uint64_t image_vmbase;   /* rewritten TEXT vmaddr (region-relative) */
    uint32_t nsegs, init_index;
    uint32_t fixup_first, fixup_count;   /* range into the global fixup array */
    struct dcc_seg segs[DCC5_MAX_SEGS];
    /* per-image rip-rel accounting lives in build-time counters + the header totals, NOT here — the
     * on-disk dcc_image layout stays byte-identical to DCC2 so the dyld reader struct matches exactly. */
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
    /* perf#24f rewrite totals (build-time gate summary) */
    uint32_t riprel_total;        /* total rip-rel sites seen in closure __text */
    uint32_t riprel_rewritten;    /* same-DATA sites rewritten */
    uint32_t riprel_sametext;     /* same-TEXT sites left unchanged */
    uint32_t riprel_skipped_pool; /* sites skipped because outside a function range (data-in-text pool) */
    /* layout: header, image_table[image_count], fixups[fixup_count], extern strtab, then 3 region blobs */
};

#endif
