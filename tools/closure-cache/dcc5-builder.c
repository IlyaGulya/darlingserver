/* perf#24f — DCC5 cache builder (region-packed + fixup table + TEXT RIP-relative REWRITE).
 *
 * Extends the DCC2 builder (dcc2-builder.c @895ceb6): identical 3-region layout + header/section-addr
 * rewrite + REBASE value translation + cache-native fixup table. THE NEW THING (perf#24f) is a
 * build-time pass that REWRITES x86-64 RIP-relative disp32 immediates in the packed __TEXT so every
 * TEXT->DATA reference (stubs, GOTPCREL, local refs) still resolves after __TEXT moved into RX and
 * __DATA moved far away into RW. Without this, DCC2 collapsed VMAs but left the disps stale => the
 * "__text" instruction-fetch SIGSEGV (root cause task #97). perf#24e proved the rewrite is bounded.
 *
 * REWRITE MATH (per rip-rel site): a site at original vmaddr V, instruction length ilen,
 * disp field disp, targets T = V+ilen+disp. After relayout, segment S moves by
 *   segDelta(S) = (new packed seg vmaddr) - (orig seg vmaddr).
 * The instruction (in __TEXT) moves by segDelta(__TEXT); the target's segment moves by segDelta(tgt_seg).
 *   newdisp = disp + segDelta(tgt_seg) - segDelta(__TEXT)
 * same-TEXT sites: segDelta equal => newdisp == disp (LEFT UNCHANGED, no patch, verified).
 * same-DATA/RO sites: differ => rewrite the disp32 in place (must fit signed int32).
 *
 * DECODER: llvm-objdump -d --arch=x86_64 --no-symbolic-operands (complete decoder, perf#24e primary
 * source of truth). We locate the disp32 field within the instruction's raw bytes by its little-endian
 * value (unambiguous for these forms), and patch the packed RX copy.
 *
 * SAFETY GATE (libsystem_m data-in-text): a dylib with an embedded FP constant pool inside __text has
 * bytes llvm-objdump flags as "bad opcode" (they are data). We parse LC_FUNCTION_STARTS to compute exact
 * function ranges and ONLY rewrite rip-rel sites whose instruction lies inside a function range; sites
 * outside all function ranges (the pool) are counted (riprel_skipped_pool) and never touched. Images with
 * NO LC_FUNCTION_STARTS but WITH undecodable non-padding bytes in __text are a hard build error.
 *
 * Deterministic (sorted list, in-order walk), atomic (tmp+rename), all-or-nothing.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <errno.h>
#include <ctype.h>
#include "dcc5-format.h"

/* ---- minimal Mach-O ---- */
struct fat_header { uint32_t magic, nfat_arch; };
struct fat_arch   { uint32_t cputype, cpusubtype, offset, size, align; };
struct mh64 { uint32_t magic,cputype,cpusubtype,filetype,ncmds,sizeofcmds,flags,reserved; };
struct lc   { uint32_t cmd, cmdsize; };
struct seg64 { uint32_t cmd,cmdsize; char segname[16]; uint64_t vmaddr,vmsize,fileoff,filesize; uint32_t maxprot,initprot,nsects,flags; };
struct sect64 { char sectname[16]; char segname[16]; uint64_t addr,size; uint32_t offset,align,reloff,nreloc,flags,reserved1,reserved2,reserved3; };
struct dylib_command { uint32_t cmd,cmdsize,name_off,timestamp,cur,compat; };
struct dyld_info { uint32_t cmd,cmdsize,rebase_off,rebase_size,bind_off,bind_size,weak_bind_off,weak_bind_size,lazy_bind_off,lazy_bind_size,export_off,export_size; };
struct linkedit_data { uint32_t cmd,cmdsize,dataoff,datasize; };
struct uuid_cmd { uint32_t cmd,cmdsize; uint8_t uuid[16]; };
#define FAT_MAGIC 0xcafebabe
#define MH_MAGIC_64 0xfeedfacf
#define CPU_TYPE_X86_64 0x01000007
#define LC_SEGMENT_64 0x19
#define LC_DYLD_INFO 0x22
#define LC_DYLD_INFO_ONLY 0x80000022
#define LC_LOAD_DYLIB 0xc
#define LC_LOAD_WEAK_DYLIB 0x80000018
#define LC_REEXPORT_DYLIB 0x8000001f
#define LC_LOAD_UPWARD_DYLIB 0x80000023
#define LC_UUID 0x1b
#define LC_FUNCTION_STARTS 0x26
#define LC_DYLD_CHAINED_FIXUPS 0x80000034

/* rebase opcodes */
#define REBASE_OP_DONE 0x00
#define REBASE_OP_SET_TYPE_IMM 0x10
#define REBASE_OP_SET_SEGMENT_AND_OFFSET_ULEB 0x20
#define REBASE_OP_ADD_ADDR_ULEB 0x30
#define REBASE_OP_ADD_ADDR_IMM_SCALED 0x40
#define REBASE_OP_DO_REBASE_IMM_TIMES 0x50
#define REBASE_OP_DO_REBASE_ULEB_TIMES 0x60
#define REBASE_OP_DO_REBASE_ADD_ADDR_ULEB 0x70
#define REBASE_OP_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB 0x80
/* bind opcodes */
#define BIND_OP_DONE 0x00
#define BIND_OP_SET_DYLIB_ORDINAL_IMM 0x10
#define BIND_OP_SET_DYLIB_ORDINAL_ULEB 0x20
#define BIND_OP_SET_DYLIB_SPECIAL_IMM 0x30
#define BIND_OP_SET_SYMBOL_TRAILING_FLAGS_IMM 0x40
#define BIND_OP_SET_TYPE_IMM 0x50
#define BIND_OP_SET_ADDEND_SLEB 0x60
#define BIND_OP_SET_SEGMENT_AND_OFFSET_ULEB 0x70
#define BIND_OP_ADD_ADDR_ULEB 0x80
#define BIND_OP_DO_BIND 0x90
#define BIND_OP_DO_BIND_ADD_ADDR_ULEB 0xA0
#define BIND_OP_DO_BIND_ADD_ADDR_IMM_SCALED 0xB0
#define BIND_OP_DO_BIND_ULEB_TIMES_SKIPPING_ULEB 0xC0
/* export trie flags */
#define EXPORT_SYMBOL_FLAGS_KIND_MASK 0x03
#define EXPORT_SYMBOL_FLAGS_REEXPORT 0x08

#define MAX_IMAGES 128
#define PTR 8

static uint64_t roundup(uint64_t v,uint64_t a){ return (v+a-1)&~(a-1); }
static uint64_t fnv1a(const void*p,size_t n,uint64_t h){ const uint8_t*b=p; for(size_t i=0;i<n;i++){h^=b[i];h*=1099511628211ULL;} return h; }
static uint64_t uleb(const uint8_t**p,const uint8_t*e){uint64_t r=0;int s=0;while(*p<e){uint8_t b=*(*p)++;r|=(uint64_t)(b&0x7f)<<s;if(!(b&0x80))break;s+=7;}return r;}
static int64_t sleb(const uint8_t**p,const uint8_t*e){int64_t r=0;int s=0;uint8_t b;do{b=*(*p)++;r|=(int64_t)(b&0x7f)<<s;s+=7;}while(b&0x80&&*p<e);if(s<64&&(b&0x40))r|=-(1LL<<s);return r;}

struct src_seg { char name[16]; uint64_t vmaddr,vmsize,fileoff,filesize; uint32_t prot; int region; };
struct func_range { uint64_t start,end; };  /* original vmaddr range of a function */
/* Executable sections that llvm-objdump does NOT reliably decode (perf#24f): __stubs (S_SYMBOL_STUBS)
 * and __stub_helper. Both carry baked RIP-relative TEXT->DATA refs that region-packing breaks and that a
 * pointer-fixup table cannot touch. Each needs a format-aware rewrite pass. */
struct stub_sec { uint64_t addr, size, fileoff; uint32_t entsize; int kind; };  /* kind: 0=__stubs 1=__stub_helper */
#define S_SYMBOL_STUBS 0x8
#define S_ATTR_PURE_INSTRUCTIONS 0x80000000u
#define S_ATTR_SOME_INSTRUCTIONS 0x00000400u
#define DCC5_MAX_STUBSECS 16
/* one executable section's coverage accounting (for the exec-coverage gate) */
struct exec_sec { char seg[16], sec[16]; uint64_t addr,size,fileoff; uint32_t flags; int decoder_blind; };
#define DCC5_MAX_EXECSECS 32
struct src_img {
    char path[256]; char hostpath[600]; uint64_t sliceOff,sliceSize; uint8_t*map; uint64_t mapsize;
    uint64_t inode,mtime,size; uint8_t uuid[16];
    struct src_seg segs[DCC5_MAX_SEGS]; int nsegs;
    uint64_t textVmaddr;                 /* original TEXT vmaddr */
    uint64_t rx_off, rw_off, ro_off;     /* region offsets for TEXT/DATA/LINKEDIT */
    struct dyld_info* di;                /* points into map (slice-relative resolved) */
    char deps[64][128]; int ndeps;       /* LC_LOAD_DYLIB leaf names, ordinal = index+1 */
    int dep_reexport[64];                /* 1 if the dep is LC_REEXPORT_DYLIB */
    int has_chained;
    /* function-starts (rip-rel rewrite safety gate) */
    struct linkedit_data* funcstarts;    /* LC_FUNCTION_STARTS command, or NULL */
    struct func_range* franges; int nfranges;
    /* __text section bounds (original vmaddr) */
    uint64_t text_addr, text_size, text_fileoff;
    /* decoder-blind rewrite sections (__stubs + __stub_helper) */
    struct stub_sec stubsecs[DCC5_MAX_STUBSECS]; int nstubsecs;
    /* all executable sections (for the zero-uncovered-exec-bytes gate) */
    struct exec_sec execsecs[DCC5_MAX_EXECSECS]; int nexecsecs;
    /* per-image rip-rel accounting */
    uint32_t riprel_rewritten, riprel_sametext, riprel_skipped_pool;
    uint32_t stub_rewritten, stub_sametext, stub_total;
    uint32_t helper_rewritten, helper_total;
    /* data-in-text (constant pool) byte offsets within __text — bytes llvm-objdump could NOT decode and
     * that are not cc/00/90 padding. The rewriter must NEVER touch these (perf#24f safety gate). */
    uint8_t* pool_bitmap;   /* text_size bytes; 1 => undecodable-pool byte */
    uint32_t pool_count;
};

static int g_red5=0;  /* RED arm 5: inject a synthetic rewrite over a pool byte; overlap-guard must abort */

static uint64_t g_rx=0,g_rw=0,g_ro=0;

/* map original (segIndex,segOffset) -> cache (region,region_off). */
static int seg_to_cache(struct src_img*im,int segIndex,uint64_t segOff,int*region,uint64_t*roff){
    if(segIndex<0||segIndex>=im->nsegs)return -1;
    struct src_seg*sg=&im->segs[segIndex];
    uint64_t base;
    if(sg->region==0)base=im->rx_off; else if(sg->region==1)base=im->rw_off; else base=im->ro_off;
    *region=sg->region; *roff=base+segOff; return 0;
}

/* ---- export trie walk (unchanged from DCC2) ---- */
static int trie_find(const uint8_t*trieStart,const uint8_t*trieEnd,const char*sym,
                     uint64_t*imgOff,int*reexportOrd,const char**reexportName){
    const uint8_t*p=trieStart; const char*s=sym;
    while(p<trieEnd){
        uint64_t terminalSize=*p++;
        if(terminalSize>127){ p--; const uint8_t*q=p; terminalSize=uleb(&q,trieEnd); p=q; }
        if(*s=='\0' && terminalSize!=0){
            const uint8_t*q=p; uint64_t flags=uleb(&q,trieEnd);
            if(flags & EXPORT_SYMBOL_FLAGS_REEXPORT){
                uint64_t ord=uleb(&q,trieEnd); const char*nm=(const char*)q;
                *reexportOrd=(int)ord; *reexportName=(nm[0]?nm:sym); return 2;
            } else {
                uint64_t off=uleb(&q,trieEnd); *imgOff=off; return 1;
            }
        }
        const uint8_t*children=p+terminalSize;
        uint8_t nchild=*children++;
        const uint8_t*c=children; int matched=0;
        for(int i=0;i<nchild;i++){
            const char*edge=(const char*)c; size_t el=strlen(edge);
            if(strncmp(s,edge,el)==0){ s+=el; const uint8_t*q=c+el+1; uint64_t nodeOff=uleb(&q,trieEnd); p=trieStart+nodeOff; matched=1; break; }
            c+=el+1; const uint8_t*q=c; uleb(&q,trieEnd); c=q;
        }
        if(!matched) return 0;
    }
    return 0;
}

/* parse LC_FUNCTION_STARTS -> function ranges (original vmaddr). Delta-encoded ULEBs from TEXT vmaddr.
 * Each function's end is the next function's start; the last ends at __text end (best available bound). */
static void parse_func_starts(struct src_img*im){
    im->franges=NULL; im->nfranges=0;
    if(!im->funcstarts || !im->funcstarts->datasize) return;
    const uint8_t*p=im->map+im->sliceOff+im->funcstarts->dataoff;
    const uint8_t*e=p+im->funcstarts->datasize;
    uint64_t addr=im->textVmaddr; int cap=256; uint64_t*starts=malloc(cap*sizeof(uint64_t)); int n=0;
    while(p<e){ uint64_t d=uleb(&p,e); if(d==0 && n>0) break; addr+=d; if(n>=cap){cap*=2;starts=realloc(starts,cap*sizeof(uint64_t));} starts[n++]=addr; }
    im->franges=malloc((n?n:1)*sizeof(struct func_range)); im->nfranges=n;
    uint64_t textEnd = im->text_addr + im->text_size;
    for(int i=0;i<n;i++){ im->franges[i].start=starts[i]; im->franges[i].end=(i+1<n)?starts[i+1]:textEnd; }
    free(starts);
}
static int in_function(struct src_img*im,uint64_t va){
    /* if no function-starts, treat everything decodable as function (only libsystem_m needs the gate). */
    if(im->nfranges==0) return 1;
    for(int i=0;i<im->nfranges;i++) if(va>=im->franges[i].start && va<im->franges[i].end) return 1;
    return 0;
}

static int parse_image(const char*hostpath, const char*guestpath, struct src_img*im){
    strncpy(im->path,guestpath,sizeof im->path-1);
    strncpy(im->hostpath,hostpath,sizeof im->hostpath-1);
    const char*path=hostpath;
    int fd=open(path,O_RDONLY); if(fd<0){fprintf(stderr,"open %s: %s\n",path,strerror(errno));return -1;}
    struct stat st; if(fstat(fd,&st)){close(fd);return -1;}
    im->inode=st.st_ino; im->mtime=st.st_mtime; im->size=st.st_size;
    uint8_t*base=mmap(0,st.st_size,PROT_READ,MAP_PRIVATE,fd,0); close(fd);
    if(base==MAP_FAILED){fprintf(stderr,"mmap %s\n",path);return -1;}
    im->map=base; im->mapsize=st.st_size;
    uint64_t so=0,ss=st.st_size; struct fat_header*fh=(void*)base;
    if(ntohl(fh->magic)==FAT_MAGIC){
        uint32_t n=ntohl(fh->nfat_arch); struct fat_arch*fa=(void*)(fh+1); int found=0;
        for(uint32_t i=0;i<n;i++) if(ntohl(fa[i].cputype)==CPU_TYPE_X86_64){so=ntohl(fa[i].offset);ss=ntohl(fa[i].size);found=1;}
        if(!found){fprintf(stderr,"%s: no x86_64 slice\n",path);return -1;}
    }
    im->sliceOff=so; im->sliceSize=ss;
    struct mh64*h=(void*)(base+so);
    if(h->magic!=MH_MAGIC_64){fprintf(stderr,"%s: not MH_MAGIC_64\n",path);return -1;}
    struct lc*c=(void*)(h+1);
    for(uint32_t i=0;i<h->ncmds;i++){
        if(c->cmd==LC_SEGMENT_64){
            struct seg64*s=(void*)c;
            if(im->nsegs>=DCC5_MAX_SEGS){fprintf(stderr,"%s: too many segs\n",path);return -1;}
            struct src_seg*d=&im->segs[im->nsegs++];
            memcpy(d->name,s->segname,16); d->vmaddr=s->vmaddr; d->vmsize=s->vmsize;
            d->fileoff=s->fileoff; d->filesize=s->filesize; d->prot=s->initprot;
            if(!strcmp(s->segname,"__TEXT")){d->region=0; im->textVmaddr=s->vmaddr;}
            else if(!strcmp(s->segname,"__LINKEDIT")) d->region=2; else d->region=1;
            /* scan sections of EVERY segment: record __text bounds, decoder-blind stub sections, and ALL
             * executable sections (for the zero-uncovered-exec-bytes gate). */
            { struct sect64*sc=(void*)((char*)s+sizeof *s);
              for(uint32_t si=0;si<s->nsects;si++,sc++){
                int is_text = (!strncmp(sc->sectname,"__text",16) && !strcmp(s->segname,"__TEXT"));
                int is_stubs = ((sc->flags & 0xff)==S_SYMBOL_STUBS);
                int is_helper = (!strncmp(sc->sectname,"__stub_helper",16));
                int has_instr = (sc->flags & (S_ATTR_PURE_INSTRUCTIONS|S_ATTR_SOME_INSTRUCTIONS))!=0;
                int executable = is_text || is_stubs || is_helper || has_instr;
                if(is_text){ im->text_addr=sc->addr; im->text_size=sc->size; im->text_fileoff=sc->offset; }
                if(is_stubs || is_helper){
                    if(im->nstubsecs>=DCC5_MAX_STUBSECS){fprintf(stderr,"%s: too many stub sections\n",path);return -1;}
                    uint32_t es = is_stubs ? (sc->reserved2 ? sc->reserved2 : 6) : 0;   /* __stubs uniform 6; helper scanned */
                    im->stubsecs[im->nstubsecs++]=(struct stub_sec){sc->addr,sc->size,sc->offset,es, is_stubs?0:1};
                }
                if(executable){
                    if(im->nexecsecs>=DCC5_MAX_EXECSECS){fprintf(stderr,"%s: too many exec sections\n",path);return -1;}
                    struct exec_sec*e=&im->execsecs[im->nexecsecs++];
                    memcpy(e->seg,s->segname,16); memcpy(e->sec,sc->sectname,16);
                    e->addr=sc->addr; e->size=sc->size; e->fileoff=sc->offset; e->flags=sc->flags;
                    e->decoder_blind = (is_stubs || is_helper);
                }
              }
            }
        } else if(c->cmd==LC_DYLD_INFO||c->cmd==LC_DYLD_INFO_ONLY){ im->di=(void*)c; }
        else if(c->cmd==LC_FUNCTION_STARTS){ im->funcstarts=(void*)c; }
        else if(c->cmd==LC_DYLD_CHAINED_FIXUPS){ im->has_chained=1; }
        else if(c->cmd==LC_LOAD_DYLIB||c->cmd==LC_LOAD_WEAK_DYLIB||c->cmd==LC_REEXPORT_DYLIB||c->cmd==LC_LOAD_UPWARD_DYLIB){
            struct dylib_command*dc=(void*)c; const char*nm=(char*)dc+dc->name_off; const char*l=strrchr(nm,'/'); l=l?l+1:nm;
            if(im->ndeps<64){ im->dep_reexport[im->ndeps]=(c->cmd==LC_REEXPORT_DYLIB); strncpy(im->deps[im->ndeps++],l,127); }
        } else if(c->cmd==LC_UUID){ struct uuid_cmd*u=(void*)c; memcpy(im->uuid,u->uuid,16); }
        c=(void*)((char*)c+c->cmdsize);
    }
    if(im->has_chained){fprintf(stderr,"%s: chained fixups not supported (closure is 100%% opcode)\n",path);return -1;}
    parse_func_starts(im);
    return 0;
}

/* globals for the whole set (for bind resolution) */
static struct src_img*g_imgs; static int g_np;
static int find_img_by_leaf(const char*leaf){
    for(int i=0;i<g_np;i++){ const char*l=strrchr(g_imgs[i].path,'/'); l=l?l+1:g_imgs[i].path; if(!strcmp(l,leaf))return i; }
    return -1;
}
static int resolve_export(int imgIdx,const char*sym,int depth,int*region,uint64_t*roff){
    if(imgIdx<0||depth>16) return 0;
    struct src_img*im=&g_imgs[imgIdx];
    if(im->di && im->di->export_size){
        const uint8_t*ts=im->map+im->sliceOff+im->di->export_off;
        const uint8_t*te=ts+im->di->export_size;
        uint64_t imgOff=0; int rord=0; const char*rname=NULL;
        int r=trie_find(ts,te,sym,&imgOff,&rord,&rname);
        if(r==1){
            for(int s=0;s<im->nsegs;s++){
                uint64_t sb=im->segs[s].vmaddr - im->textVmaddr;
                uint64_t se=sb+im->segs[s].vmsize;
                if(imgOff>=sb && imgOff<se){ uint64_t inSeg=imgOff-sb; return seg_to_cache(im,s,inSeg,region,roff)==0; }
            }
            return 0;
        } else if(r==2){
            if(rord>=1 && rord<=im->ndeps){ int tgt=find_img_by_leaf(im->deps[rord-1]); if(resolve_export(tgt,rname,depth+1,region,roff))return 1; }
        }
    }
    for(int d=0;d<im->ndeps;d++){
        if(!im->dep_reexport[d]) continue;
        int tgt=find_img_by_leaf(im->deps[d]);
        if(tgt>=0 && resolve_export(tgt,sym,depth+1,region,roff)) return 1;
    }
    return 0;
}
static int resolve_flat(const char*sym,int*region,uint64_t*roff,int*whichImg){
    for(int i=0;i<g_np;i++){ if(resolve_export(i,sym,0,region,roff)){ *whichImg=i; return 1; } }
    return 0;
}

/* ---- fixup emission (unchanged from DCC2) ---- */
struct dcc_fixup* g_fix=NULL; uint32_t g_nfix=0,g_capfix=0;
char* g_extern=NULL; uint32_t g_extlen=0,g_extcap=0;
static uint32_t extern_add(const char*s){
    uint32_t need=strlen(s)+1;
    if(g_extlen+need>g_extcap){ g_extcap=(g_extcap?g_extcap*2:4096); while(g_extlen+need>g_extcap)g_extcap*=2; g_extern=realloc(g_extern,g_extcap); }
    uint32_t off=g_extlen; memcpy(g_extern+off,s,need); g_extlen+=need; return off;
}
static struct dcc_fixup* fix_new(void){
    if(g_nfix>=g_capfix){ g_capfix=g_capfix?g_capfix*2:65536; g_fix=realloc(g_fix,g_capfix*sizeof *g_fix); }
    struct dcc_fixup*f=&g_fix[g_nfix++]; memset(f,0,sizeof *f); return f;
}
static int c_rebase=0,c_bind_internal=0,c_bind_self=0,c_bind_flat_resolved=0,c_bind_extern=0,c_unresolved=0;
static void emit_rebase(struct src_img*im,int imgIdx,int segIndex,uint64_t segOff){
    int reg; uint64_t roff;
    if(seg_to_cache(im,segIndex,segOff,&reg,&roff)){ fprintf(stderr,"ABORT rebase seg map %s\n",im->path); exit(1);}
    struct dcc_fixup*f=fix_new(); f->kind=DCC_FIX_REBASE; f->loc_region=reg; f->loc_off=roff; f->image_index=imgIdx;
    c_rebase++;
}
static int c_lazy_rebase=0;
static void emit_bind(struct src_img*im,int imgIdx,int segIndex,uint64_t segOff,const char*sym,int ord,int64_t addend,int isLazy){
    int reg; uint64_t roff;
    if(seg_to_cache(im,segIndex,segOff,&reg,&roff)){ fprintf(stderr,"ABORT bind seg map %s\n",im->path); exit(1);}
    struct dcc_fixup*f=fix_new(); f->loc_region=reg; f->loc_off=roff; f->addend=addend; f->image_index=imgIdx;
    int tr; uint64_t troff; int done=0;
    if(ord>=1){ if(ord<=im->ndeps){ int tgt=find_img_by_leaf(im->deps[ord-1]); if(resolve_export(tgt,sym,0,&tr,&troff)){ f->kind=DCC_FIX_BIND_INTERNAL; f->tgt_region=tr; f->tgt_off=troff; c_bind_internal++; done=1; } } }
    else if(ord==0){ if(resolve_export(imgIdx,sym,0,&tr,&troff)){ f->kind=DCC_FIX_BIND_INTERNAL; f->tgt_region=tr; f->tgt_off=troff; c_bind_self++; done=1; } }
    else if(ord==-2){ int w; if(resolve_flat(sym,&tr,&troff,&w)){ f->kind=DCC_FIX_BIND_INTERNAL; f->tgt_region=tr; f->tgt_off=troff; c_bind_flat_resolved++; done=1; } }
    if(!done){
        if(isLazy){ f->kind=DCC_FIX_BIND_EXTERN_LAZY; f->extern_sym=extern_add(sym); c_lazy_rebase++; return; }
        f->kind=DCC_FIX_BIND_EXTERN; f->extern_sym=extern_add(sym); c_bind_extern++;
        if(ord>=1 || ord==0){ c_unresolved++;
            if(getenv("DCC_DEBUG")){ const char*dep=(ord>=1&&ord<=im->ndeps)?im->deps[ord-1]:"<self>";
                fprintf(stderr,"  UNRESOLVED %s ord=%d(%s) sym=%s\n", strrchr(im->path,'/')+1, ord, dep, sym); } }
    }
}
static void walk_rebases(struct src_img*im,int imgIdx){
    if(!im->di||!im->di->rebase_size)return;
    const uint8_t*p=im->map+im->sliceOff+im->di->rebase_off, *e=p+im->di->rebase_size;
    int segIndex=0; uint64_t segOff=0;
    while(p<e){ uint8_t op=*p++; uint8_t im4=op&0x0f; op&=0xf0;
        switch(op){
            case REBASE_OP_DONE: return;
            case REBASE_OP_SET_TYPE_IMM: break;
            case REBASE_OP_SET_SEGMENT_AND_OFFSET_ULEB: segIndex=im4; segOff=uleb(&p,e); break;
            case REBASE_OP_ADD_ADDR_ULEB: segOff+=uleb(&p,e); break;
            case REBASE_OP_ADD_ADDR_IMM_SCALED: segOff+=(uint64_t)im4*PTR; break;
            case REBASE_OP_DO_REBASE_IMM_TIMES: for(int i=0;i<im4;i++){ emit_rebase(im,imgIdx,segIndex,segOff); segOff+=PTR; } break;
            case REBASE_OP_DO_REBASE_ULEB_TIMES: { uint64_t cnt=uleb(&p,e); for(uint64_t i=0;i<cnt;i++){ emit_rebase(im,imgIdx,segIndex,segOff); segOff+=PTR; } } break;
            case REBASE_OP_DO_REBASE_ADD_ADDR_ULEB: { emit_rebase(im,imgIdx,segIndex,segOff); segOff+=PTR+uleb(&p,e); } break;
            case REBASE_OP_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB: { uint64_t cnt=uleb(&p,e); uint64_t skip=uleb(&p,e); for(uint64_t i=0;i<cnt;i++){ emit_rebase(im,imgIdx,segIndex,segOff); segOff+=PTR+skip; } } break;
            default: fprintf(stderr,"ABORT unknown rebase op 0x%x in %s\n",op,im->path); exit(1);
        }
    }
}
static void walk_bind_stream(struct src_img*im,int imgIdx,const uint8_t*p,const uint8_t*e,int isLazy){
    int segIndex=0; uint64_t segOff=0; int ord=0; const char*sym=""; int64_t addend=0;
    while(p<e){ uint8_t op=*p++; uint8_t im4=op&0x0f; op&=0xf0;
        switch(op){
            case BIND_OP_DONE: if(!isLazy) return; break;
            case BIND_OP_SET_DYLIB_ORDINAL_IMM: ord=im4; break;
            case BIND_OP_SET_DYLIB_ORDINAL_ULEB: ord=(int)uleb(&p,e); break;
            case BIND_OP_SET_DYLIB_SPECIAL_IMM: ord=(im4==0)?0:(int8_t)(im4|0xf0); break;
            case BIND_OP_SET_SYMBOL_TRAILING_FLAGS_IMM: sym=(const char*)p; while(*p)p++; p++; break;
            case BIND_OP_SET_TYPE_IMM: break;
            case BIND_OP_SET_ADDEND_SLEB: addend=sleb(&p,e); break;
            case BIND_OP_SET_SEGMENT_AND_OFFSET_ULEB: segIndex=im4; segOff=uleb(&p,e); break;
            case BIND_OP_ADD_ADDR_ULEB: segOff+=uleb(&p,e); break;
            case BIND_OP_DO_BIND: emit_bind(im,imgIdx,segIndex,segOff,sym,ord,addend,isLazy); segOff+=PTR; break;
            case BIND_OP_DO_BIND_ADD_ADDR_ULEB: emit_bind(im,imgIdx,segIndex,segOff,sym,ord,addend,isLazy); segOff+=PTR+uleb(&p,e); break;
            case BIND_OP_DO_BIND_ADD_ADDR_IMM_SCALED: emit_bind(im,imgIdx,segIndex,segOff,sym,ord,addend,isLazy); segOff+=PTR+(uint64_t)im4*PTR; break;
            case BIND_OP_DO_BIND_ULEB_TIMES_SKIPPING_ULEB: { uint64_t cnt=uleb(&p,e); uint64_t skip=uleb(&p,e); for(uint64_t i=0;i<cnt;i++){ emit_bind(im,imgIdx,segIndex,segOff,sym,ord,addend,isLazy); segOff+=PTR+skip; } } break;
            default: fprintf(stderr,"ABORT unknown bind op 0x%x in %s\n",op,im->path); exit(1);
        }
    }
}
static void walk_binds(struct src_img*im,int imgIdx){
    if(!im->di) return;
    if(im->di->bind_size)      walk_bind_stream(im,imgIdx, im->map+im->sliceOff+im->di->bind_off,      im->map+im->sliceOff+im->di->bind_off+im->di->bind_size, 0);
    if(im->di->lazy_bind_size) walk_bind_stream(im,imgIdx, im->map+im->sliceOff+im->di->lazy_bind_off, im->map+im->sliceOff+im->di->lazy_bind_off+im->di->lazy_bind_size, 1);
    if(im->di->weak_bind_size) walk_bind_stream(im,imgIdx, im->map+im->sliceOff+im->di->weak_bind_off, im->map+im->sliceOff+im->di->weak_bind_off+im->di->weak_bind_size, 0);
}

/* ================= perf#24f: RIP-relative TEXT rewrite ================= */
/* Global segment-delta lookup: for an original vmaddr in image im, return the packed vmaddr. */
static int seg_index_of_va(struct src_img*im,uint64_t va){
    for(int s=0;s<im->nsegs;s++) if(va>=im->segs[s].vmaddr && va<im->segs[s].vmaddr+im->segs[s].vmsize) return s;
    return -1;
}
/* packed seg delta = new packed seg vmaddr - orig seg vmaddr. */
static int64_t seg_delta(struct src_img*im,int s,uint64_t RXb,uint64_t RWb,uint64_t ROb){
    uint64_t base = (im->segs[s].region==0)?im->rx_off : (im->segs[s].region==1)?im->rw_off : im->ro_off;
    uint64_t vmb  = (im->segs[s].region==0)?RXb : (im->segs[s].region==1)?RWb : ROb;
    return (int64_t)(vmb+base) - (int64_t)im->segs[s].vmaddr;
}

/* Compute the data-in-text (constant pool) byte bitmap for an image: bytes inside __text that
 * llvm-objdump does NOT decode as instructions and are not cc/00/90 padding. These are the libsystem_m
 * FP constant-pool bytes (perf#24e). The rewriter must never touch them. Returns 0 ok. */
static int detect_pool(struct src_img*im){
    if(!im->text_size) return 0;
    im->pool_bitmap=calloc(1,im->text_size); im->pool_count=0;
    /* mark decoded instruction bytes */
    uint8_t* decoded=calloc(1,im->text_size);
    char cmd[900]; snprintf(cmd,sizeof cmd,"llvm-objdump -d --arch=x86_64 --no-symbolic-operands '%s' 2>/dev/null",im->hostpath);
    FILE*pp=popen(cmd,"r"); if(!pp){fprintf(stderr,"ABORT popen objdump(pool) %s\n",im->path);free(decoded);return 1;}
    char line[4096];
    while(fgets(line,sizeof line,pp)){
        char*colon=strchr(line,':'); if(!colon) continue;
        int allhex=1; for(char*q=line;q<colon;q++){ if(!isspace((unsigned char)*q)&&!isxdigit((unsigned char)*q)){allhex=0;break;} }
        if(!allhex) continue;
        uint64_t va=strtoull(line,NULL,16);
        if(va<im->text_addr || va>=im->text_addr+im->text_size) continue;
        char*after=colon+1; while(*after=='\t'||*after==' ')after++;
        char*tab=strchr(after,'\t'); if(!tab) continue; *tab=0;
        int nraw=0; { char*tok=strtok(after," "); while(tok){ nraw++; tok=strtok(NULL," "); } }
        uint64_t o=va-im->text_addr;
        for(int k=0;k<nraw && o+k<im->text_size;k++) decoded[o+k]=1;
    }
    pclose(pp);
    /* undecoded, non-padding bytes = pool */
    const uint8_t* txt=im->map+im->sliceOff+im->text_fileoff;
    for(uint64_t o=0;o<im->text_size;o++){
        if(decoded[o]) continue;
        uint8_t b=txt[o];
        if(b==0xcc||b==0x00||b==0x90) continue;  /* alignment padding */
        im->pool_bitmap[o]=1; im->pool_count++;
    }
    free(decoded);
    return 0;
}

/* Rewrite one image's packed __text. rxblob = pointer to this image's packed TEXT bytes (RX region blob
 * at rx_file+rx_off). Returns 0 ok, nonzero on hard error (int32 overflow, undecodable non-pool bytes). */
struct riprel_gate { int int32_overflow, undecodable_nonpool, target_class_bad, pool_overlap; };
static int rewrite_image_text(struct src_img*im,int imgIdx,uint8_t*rxblob,
                              uint64_t RXb,uint64_t RWb,uint64_t ROb,struct riprel_gate*gate){
    /* llvm-objdump the HOST file (deterministic). Parse each line: "  <va>:\t<hex bytes>\t<asm>". */
    char cmd[900];
    snprintf(cmd,sizeof cmd,"llvm-objdump -d --arch=x86_64 --no-symbolic-operands '%s' 2>/dev/null",im->hostpath);
    FILE*pp=popen(cmd,"r"); if(!pp){fprintf(stderr,"ABORT popen objdump %s\n",im->path);return 1;}
    char line[4096];
    int64_t dText=seg_delta(im,0,RXb,RWb,ROb);  /* seg 0 == __TEXT */
    while(fgets(line,sizeof line,pp)){
        /* find "<hex>:\t" prefix */
        char*colon=strchr(line,':'); if(!colon) continue;
        /* verify everything before colon is hex */
        int allhex=1; for(char*q=line;q<colon;q++){ if(!isspace((unsigned char)*q)&&!isxdigit((unsigned char)*q)){allhex=0;break;} }
        if(!allhex) continue;
        char*after=colon+1; while(*after=='\t'||*after==' ')after++;
        uint64_t va=strtoull(line,NULL,16);
        /* only within __text */
        if(va<im->text_addr || va>=im->text_addr+im->text_size) continue;
        /* raw bytes field: up to next tab */
        char*tab=strchr(after,'\t'); if(!tab) continue;
        *tab=0; char*asmp=tab+1;
        /* parse raw hex bytes */
        uint8_t raw[32]; int nraw=0; { char*tok=strtok(after," "); while(tok&&nraw<32){ raw[nraw++]=(uint8_t)strtoul(tok,NULL,16); tok=strtok(NULL," "); } }
        if(nraw==0) continue;
        /* does the asm contain a "(%rip)" operand? if so extract its disp. */
        char*rip=strstr(asmp,"(%rip)"); if(!rip) continue;
        /* The disp literal ends exactly at `rip` and has the form [-]0x<hex>. Scan backward over the hex
         * digits, then the "0x", then an optional '-'. Do NOT rely on a delimiter stop-set: forms like
         * `jmpq *0x53a42(%rip)` put a '*' immediately before the disp, and strtoll("*0x..") returns 0,
         * silently misclassifying every stub as same-TEXT (perf#24f caught 676 such sites). */
        char*he=rip;                 /* one past last hex digit */
        char*hs=he; while(hs>asmp && isxdigit((unsigned char)*(hs-1))) hs--;   /* start of hex digits */
        if(hs<asmp+2 || *(hs-1)!='x' || *(hs-2)!='0'){ fprintf(stderr,"ABORT %s va=0x%llx malformed rip disp: %s\n",im->path,(unsigned long long)va,asmp); pclose(pp); return 1; }
        char*ds=hs-2;                /* at '0' of "0x" */
        int neg=(ds>asmp && *(ds-1)=='-');
        char buf[40]; int bl=(int)(he-ds); if(bl<=0||bl>36){ fprintf(stderr,"ABORT %s va=0x%llx disp too long\n",im->path,(unsigned long long)va); pclose(pp); return 1; }
        memcpy(buf,ds,bl); buf[bl]=0;
        long long disp=strtoll(buf,NULL,16); if(neg) disp=-disp;
        uint64_t ilen=nraw; uint64_t nextva=va+ilen; uint64_t tgt=nextva+(int64_t)disp;
        /* classify target */
        int tseg=seg_index_of_va(im,tgt);
        if(tseg<0){
            /* target not in any segment: perf#24e showed these were section padding inside __TEXT that
             * seg_of DID find; if truly outside all segments it's an error class. */
            gate->target_class_bad++;
            fprintf(stderr,"ABORT %s rip site va=0x%llx tgt=0x%llx outside all segments\n",im->path,(unsigned long long)va,(unsigned long long)tgt);
            pclose(pp); return 1;
        }
        int64_t dTgt=seg_delta(im,tseg,RXb,RWb,ROb);
        if(dTgt==dText){ im->riprel_sametext++; continue; }  /* same-move (same-TEXT) => leave */
        /* safety gate: only rewrite if the instruction lies inside a function range (excludes data-in-text). */
        if(!in_function(im,va)){ im->riprel_skipped_pool++; continue; }
        /* compute new disp */
        int64_t newdisp = (int64_t)disp + (dTgt - dText);
        if(newdisp < -(1LL<<31) || newdisp >= (1LL<<31)){ gate->int32_overflow++;
            fprintf(stderr,"ABORT %s rip site va=0x%llx int32 overflow newdisp=0x%llx\n",im->path,(unsigned long long)va,(unsigned long long)newdisp); pclose(pp); return 1; }
        /* Locate the disp32 field within raw[] by its little-endian value. perf#24f verified over the whole
         * closure (ripformcheck): the disp32 LE value appears at EXACTLY ONE byte position in every rip-rel
         * instruction (0 ambiguous), even though 2010 sites carry a trailing immediate (so the disp is NOT
         * always the last 4 bytes). We therefore require a UNIQUE match and hard-abort on 0 or >1 — a >1
         * ambiguity would risk a silent mispatch, so it is a build error, not a guess. */
        uint32_t old32=(uint32_t)(int32_t)disp; int fpos=-1,nmatch=0;
        for(int b=0;b+4<=nraw;b++){ uint32_t v=raw[b]|(raw[b+1]<<8)|(raw[b+2]<<16)|((uint32_t)raw[b+3]<<24); if(v==old32){ fpos=b; nmatch++; } }
        if(nmatch!=1){ fprintf(stderr,"ABORT %s va=0x%llx disp32 0x%x located %d times in raw bytes (need exactly 1)\n",
            im->path,(unsigned long long)va,old32,nmatch); pclose(pp); return 1; }
        /* patch the packed RX copy: byte offset within __text = va - text_addr; plus fpos into the instruction */
        uint64_t off_in_text = va - im->text_addr;
        /* RED arm 5: pretend this rewrite lands on the constant pool (treat pool as code). The overlap
         * guard below MUST abort. We shift the write target onto the first pool byte of this image. */
        if(g_red5 && im->pool_count){ for(uint64_t o=0;o<im->text_size;o++) if(im->pool_bitmap[o]){ off_in_text=o; fpos=0; break; } }
        /* SAFETY GATE: the 4 disp bytes must lie entirely inside decoded instruction bytes, never on a
         * data-in-text constant-pool byte. This is what makes "libsystem_m pool treated as code" a
         * detected error rather than silent corruption. */
        for(int k=0;k<4;k++){ uint64_t o=off_in_text+fpos+k;
            if(o<im->text_size && im->pool_bitmap[o]){ gate->pool_overlap++;
                fprintf(stderr,"ABORT %s rewrite at __text+0x%llx overlaps constant-pool byte 0x%llx\n",
                    im->path,(unsigned long long)(off_in_text+fpos),(unsigned long long)o); pclose(pp); return 1; } }
        uint8_t*loc = rxblob + (im->text_fileoff - im->segs[0].fileoff) + off_in_text + fpos;
        /* NOTE rxblob points at the packed TEXT segment start (fileoff of __TEXT). __text section is at
         * text_fileoff within the file; within the segment its offset is text_fileoff - TEXT.fileoff. */
        uint32_t new32=(uint32_t)(int32_t)newdisp;
        loc[0]=new32&0xff; loc[1]=(new32>>8)&0xff; loc[2]=(new32>>16)&0xff; loc[3]=(new32>>24)&0xff;
        im->riprel_rewritten++;
    }
    pclose(pp);
    return 0;
}

/* ===== perf#24f: MANDATORY rewrite of decoder-blind executable sections (__stubs + __stub_helper) =====
 * llvm-objdump does NOT decode S_SYMBOL_STUBS, and treats __stub_helper as data too. Both carry baked
 * RIP-relative TEXT->DATA refs (stub `ff25 jmp*`, helper head `4c8d1d lea r11` + `ff25 jmp*dyld_stub_binder`)
 * that region-packing breaks. Format-aware passes rewrite them; unknown pattern in an executable section
 * => HARD ABORT (never best-effort). */
static int g_redstub=0;   /* 1=skip one stub, 2=wrong disp on one stub, 3=corrupt one stub's opcode */
static int g_nostubs=0;   /* RED arm: skip __stubs pass entirely */
static int g_nohelper=0;  /* RED arm: skip __stub_helper pass entirely */
static int g_redhelper=0; /* 1=skip one helper rip rewrite */

/* Rewrite one RIP-relative disp32 at packed offset `poff_in_seg` (disp field), whose instruction ends at
 * original vmaddr `insn_end_va` and targets `tgt_va`. Verifies the new target lands in the expected region.
 * Returns 0 ok (and sets *rewritten=1 if a same-DATA/RO rewrite happened, 0 if same-TEXT left as-is). */
static int rewrite_rip_field(struct src_img*im,uint64_t RXb,uint64_t RWb,uint64_t ROb,
                             uint8_t*rxblob,uint64_t poff_field,uint64_t insn_end_va,uint64_t tgt_va,
                             int32_t olddisp,int forced_bad_disp,struct riprel_gate*gate,int*rewritten){
    int64_t dText=seg_delta(im,0,RXb,RWb,ROb);
    int tseg=seg_index_of_va(im,tgt_va);
    if(tseg<0){ gate->target_class_bad++; fprintf(stderr,"ABORT %s rip end=0x%llx tgt=0x%llx outside all segments\n",
        im->path,(unsigned long long)insn_end_va,(unsigned long long)tgt_va); return 1; }
    int64_t dTgt=seg_delta(im,tseg,RXb,RWb,ROb);
    if(dTgt==dText){ *rewritten=0; return 0; }   /* same-move: leave unchanged */
    int64_t newdisp=(int64_t)olddisp + (dTgt-dText) + forced_bad_disp;
    if(newdisp < -(1LL<<31) || newdisp >= (1LL<<31)){ gate->int32_overflow++;
        fprintf(stderr,"ABORT %s rip end=0x%llx int32 overflow\n",im->path,(unsigned long long)insn_end_va); return 1; }
    uint32_t new32=(uint32_t)(int32_t)newdisp;
    uint8_t*loc=rxblob+poff_field;
    loc[0]=new32&0xff; loc[1]=(new32>>8)&0xff; loc[2]=(new32>>16)&0xff; loc[3]=(new32>>24)&0xff;
    /* verify: packed insn_end + new disp lands exactly on the moved target */
    uint64_t packed_insn_end = (RXb+im->rx_off) + (insn_end_va - im->textVmaddr);
    uint64_t new_tgt = packed_insn_end + (int64_t)(int32_t)new32;
    uint64_t tbase=(im->segs[tseg].region==0)?RXb:(im->segs[tseg].region==1)?RWb:ROb;
    uint64_t toff =(im->segs[tseg].region==0)?im->rx_off:(im->segs[tseg].region==1)?im->rw_off:im->ro_off;
    uint64_t exp = tbase+toff+(tgt_va-im->segs[tseg].vmaddr);
    if(!forced_bad_disp && new_tgt!=exp){ fprintf(stderr,"ABORT %s rip end=0x%llx new tgt 0x%llx != exp 0x%llx\n",
        im->path,(unsigned long long)insn_end_va,(unsigned long long)new_tgt,(unsigned long long)exp); return 1; }
    *rewritten=1; return 0;
}

/* __stubs: uniform 6-byte `ff 25 disp32` (or ff 15). */
static int rewrite_stubs_section(struct src_img*im,uint64_t RXb,uint64_t RWb,uint64_t ROb,uint8_t*rxblob,
                                 struct stub_sec*S,struct riprel_gate*gate,int*red_done){
    if(S->entsize!=6){ fprintf(stderr,"ABORT %s __stubs entsize=%u (must be 6)\n",im->path,S->entsize); return 1; }
    uint64_t base_off=(S->fileoff - im->segs[0].fileoff);
    for(uint64_t k=0,n=S->size/S->entsize;k<n;k++){
        uint64_t stub_va=S->addr+k*S->entsize;
        const uint8_t*o=im->map+im->sliceOff+S->fileoff+k*S->entsize;
        im->stub_total++;
        if(!(o[0]==0xff && (o[1]==0x25||o[1]==0x15))){ fprintf(stderr,"ABORT %s __stubs@0x%llx unknown %02x%02x\n",im->path,(unsigned long long)stub_va,o[0],o[1]); return 1; }
        int32_t disp=(int32_t)(o[2]|(o[3]<<8)|(o[4]<<16)|((uint32_t)o[5]<<24));
        uint64_t poff_field=base_off+k*S->entsize+2; uint64_t end=stub_va+6; uint64_t tgt=end+(int64_t)disp;
        int fb=0; if(g_redstub && !*red_done){ if(g_redstub==1){*red_done=1;im->stub_rewritten++;continue;} if(g_redstub==2){fb=0x20;*red_done=1;} if(g_redstub==3){ rxblob[base_off+k*S->entsize+1]=0x99; *red_done=1; } }
        int rw=0; if(rewrite_rip_field(im,RXb,RWb,ROb,rxblob,poff_field,end,tgt,disp,fb,gate,&rw)) return 1;
        if(rw) im->stub_rewritten++; else im->stub_sametext++;
    }
    return 0;
}

/* __stub_helper: scan for known RIP-relative forms; rewrite ff25/ff15 (indirect jmp and call) and the
 * lea disp(%rip),reg forms (4c8d/488d with modrm rm=101). Everything else must be a known non-rip filler:
 * 41 53 (push r11), 68 imm32 (push), e9 rel32 (jmp rel, TEXT-internal), 90/cc/00 padding. Any other byte
 * in an executable section => HARD ABORT. */
static int rewrite_stub_helper(struct src_img*im,uint64_t RXb,uint64_t RWb,uint64_t ROb,uint8_t*rxblob,
                               struct stub_sec*S,struct riprel_gate*gate,int*red_done){
    uint64_t base_off=(S->fileoff - im->segs[0].fileoff);
    const uint8_t*b=im->map+im->sliceOff+S->fileoff;
    uint64_t i=0, n=S->size;
    while(i<n){
        uint64_t va=S->addr+i; uint8_t c=b[i];
        im->helper_total++;
        if(c==0x90||c==0xcc||c==0x00){ i++; continue; }                 /* padding */
        if(c==0x41 && i+1<n && b[i+1]==0x53){ i+=2; continue; }          /* push %r11 */
        if(c==0x68 && i+5<=n){ i+=5; continue; }                        /* push imm32 (lazy bind index) */
        if(c==0xe9 && i+5<=n){ i+=5; continue; }                        /* jmp rel32 (TEXT-internal, moves rigidly) */
        /* lea disp32(%rip),reg : REX.W (48/4c) + 8d + modrm with mod=00,rm=101 */
        if((c==0x48||c==0x4c) && i+7<=n && b[i+1]==0x8d && (b[i+2]&0xc7)==0x05){
            int32_t disp=(int32_t)(b[i+3]|(b[i+4]<<8)|(b[i+5]<<16)|((uint32_t)b[i+6]<<24));
            uint64_t end=va+7, tgt=end+(int64_t)disp; uint64_t poff=base_off+i+3;
            int fb=0; if(g_redhelper && !*red_done){ *red_done=1; fb=0x40; }
            int rw=0; if(rewrite_rip_field(im,RXb,RWb,ROb,rxblob,poff,end,tgt,disp,fb,gate,&rw)) return 1;
            if(rw) im->helper_rewritten++; i+=7; continue;
        }
        /* indirect jmp/call disp32(%rip): ff /4 or ff /2, modrm mod=00 rm=101 => ff 25 / ff 15 */
        if(c==0xff && i+6<=n && (b[i+1]==0x25||b[i+1]==0x15)){
            int32_t disp=(int32_t)(b[i+2]|(b[i+3]<<8)|(b[i+4]<<16)|((uint32_t)b[i+5]<<24));
            uint64_t end=va+6, tgt=end+(int64_t)disp; uint64_t poff=base_off+i+2;
            int fb=0; if(g_redhelper && !*red_done){ *red_done=1; fb=0x40; }
            int rw=0; if(rewrite_rip_field(im,RXb,RWb,ROb,rxblob,poff,end,tgt,disp,fb,gate,&rw)) return 1;
            if(rw) im->helper_rewritten++; i+=6; continue;
        }
        fprintf(stderr,"ABORT %s __stub_helper@0x%llx unknown executable byte 0x%02x (offset %llu)\n",
            im->path,(unsigned long long)va,c,(unsigned long long)i); return 1;
    }
    return 0;
}

static int rewrite_image_stubs(struct src_img*im,int imgIdx,uint8_t*rxblob,
                               uint64_t RXb,uint64_t RWb,uint64_t ROb,struct riprel_gate*gate){
    int red_done=0;
    for(int ss=0; ss<im->nstubsecs; ss++){
        struct stub_sec*S=&im->stubsecs[ss];
        if(S->kind==0){ if(g_nostubs) continue;  if(rewrite_stubs_section(im,RXb,RWb,ROb,rxblob,S,gate,&red_done)) return 1; }
        else          { if(g_nohelper) continue; if(rewrite_stub_helper(im,RXb,RWb,ROb,rxblob,S,gate,&red_done)) return 1; }
    }
    return 0;
}

int main(int argc,char**argv){
    const char*root=NULL,*listf=NULL,*outf=NULL;
    for(int i=1;i<argc;i++){ if(!strcmp(argv[i],"--red5")) g_red5=1;
        else if(!strncmp(argv[i],"--redstub=",10)) g_redstub=atoi(argv[i]+10);
        else if(!strcmp(argv[i],"--no-stubs")) g_nostubs=1;    /* RED arm: __stubs UNrewritten */
        else if(!strcmp(argv[i],"--no-helper")) g_nohelper=1;  /* RED arm: __stub_helper UNrewritten */
        else if(!strcmp(argv[i],"--redhelper")) g_redhelper=1; /* RED arm: skip one helper rip rewrite */
        else if(!root)root=argv[i]; else if(!listf)listf=argv[i]; else outf=argv[i]; }
    if(!root||!listf||!outf){fprintf(stderr,"usage: %s [--red5|--redstub=N|--no-stubs|--no-helper|--redhelper] <install_root> <closure_list.txt> <out.dcc5>\n",argv[0]);return 2;}

    FILE*lf=fopen(listf,"r"); if(!lf){perror("list");return 2;}
    char*paths[MAX_IMAGES]; int np=0; char line[512];
    while(fgets(line,sizeof line,lf)){ char*nl=strchr(line,'\n'); if(nl)*nl=0; if(!line[0])continue; if(!strstr(line,".dylib"))continue; paths[np++]=strdup(line); if(np>=MAX_IMAGES)break; }
    fclose(lf);
    for(int i=0;i<np;i++)for(int j=i+1;j<np;j++) if(strcmp(paths[i],paths[j])>0){char*t=paths[i];paths[i]=paths[j];paths[j]=t;}

    struct src_img*imgs=calloc(np,sizeof *imgs); g_imgs=imgs; g_np=np;
    uint64_t chash=1469598103934665603ULL;
    for(int i=0;i<np;i++){ char full[600]; snprintf(full,sizeof full,"%s%s",root,paths[i]);
        if(parse_image(full,paths[i],&imgs[i])){fprintf(stderr,"ABORT parse %s\n",full);return 1;}
        chash=fnv1a(paths[i],strlen(paths[i]),chash); chash=fnv1a(&imgs[i].inode,8,chash); chash=fnv1a(&imgs[i].mtime,8,chash); chash=fnv1a(&imgs[i].size,8,chash);
    }

    /* region offsets (first-fit, page-aligned per seg) */
    for(int i=0;i<np;i++){ struct src_img*im=&imgs[i];
        for(int s=0;s<im->nsegs;s++){ uint64_t sz=roundup(im->segs[s].vmsize,DCC5_REGION_ALIGN);
            if(im->segs[s].region==0){im->rx_off=g_rx;g_rx+=sz;} else if(im->segs[s].region==1){im->rw_off=g_rw;g_rw+=sz;} else {im->ro_off=g_ro;g_ro+=sz;} }
    }
    uint64_t RXb=0, RWb=roundup(g_rx,DCC5_REGION_ALIGN), ROb=roundup(RWb+g_rw,DCC5_REGION_ALIGN);

    /* fixup table */
    for(int i=0;i<np;i++){ walk_rebases(&imgs[i],i); walk_binds(&imgs[i],i); }

    struct dcc_header hdr; memset(&hdr,0,sizeof hdr);
    /* perf#24f-#107: DCC6 = DCC5 layout + per-seg orig_vmaddr/orig_vmsize (dcc_seg grew). */
    hdr.magic=DCC6_MAGIC; hdr.version=DCC6_VERSION; hdr.arch=CPU_TYPE_X86_64; hdr.image_count=np;
    hdr.closure_hash=chash; strncpy(hdr.install_root,root,sizeof hdr.install_root-1);

    uint64_t tbl_off=sizeof(struct dcc_header);
    uint64_t images_bytes=(uint64_t)np*sizeof(struct dcc_image);
    uint64_t fixup_off=tbl_off+images_bytes;
    uint64_t fixup_bytes=(uint64_t)g_nfix*sizeof(struct dcc_fixup);
    uint64_t extern_off=fixup_off+fixup_bytes;
    uint64_t extern_bytes=g_extlen;
    uint64_t rx_file=roundup(extern_off+extern_bytes,DCC5_REGION_ALIGN);
    uint64_t rw_file=roundup(rx_file+g_rx,DCC5_REGION_ALIGN);
    uint64_t ro_file=roundup(rw_file+g_rw,DCC5_REGION_ALIGN);
    uint64_t total=roundup(ro_file+g_ro,DCC5_REGION_ALIGN);

    hdr.regions[0]=(struct dcc_region){rx_file,g_rx,RXb,5,0};
    hdr.regions[1]=(struct dcc_region){rw_file,g_rw,RWb,3,0};
    hdr.regions[2]=(struct dcc_region){ro_file,g_ro,ROb,1,0};
    hdr.fixup_off=fixup_off; hdr.fixup_count=g_nfix;
    hdr.extern_off=extern_off; hdr.extern_size=g_extlen;

    uint8_t*out=calloc(1,total); memcpy(out,&hdr,sizeof hdr);
    struct dcc_image*tbl=(void*)(out+tbl_off);

    for(int i=0;i<np;i++){ struct src_img*im=&imgs[i]; struct dcc_image*di=&tbl[i];
        strncpy(di->path,im->path,sizeof di->path-1); memcpy(di->uuid,im->uuid,16);
        di->src_inode=im->inode; di->src_mtime=im->mtime; di->src_size=im->size;
        di->image_vmbase=RXb+im->rx_off; di->nsegs=im->nsegs; di->init_index=i;
        for(int s=0;s<im->nsegs;s++){ struct src_seg*sg=&im->segs[s]; struct dcc_seg*ds=&di->segs[s];
            memcpy(ds->name,sg->name,16); ds->vmsize=sg->vmsize; ds->filesize=sg->filesize; ds->prot=sg->prot;
            /* perf#24f-#107: record ORIGINAL seg vmaddr/vmsize so the reader can translate an
             * export-trie address (original-image-relative) to the rewritten region-relative arena. */
            ds->orig_vmaddr=sg->vmaddr; ds->orig_vmsize=sg->vmsize;
            uint64_t roff,vmb,rfile; int ridx=sg->region;
            if(ridx==0){roff=im->rx_off;vmb=RXb;rfile=rx_file;} else if(ridx==1){roff=im->rw_off;vmb=RWb;rfile=rw_file;} else {roff=im->ro_off;vmb=ROb;rfile=ro_file;}
            ds->region_idx=ridx; ds->region_off=roff; ds->vmaddr=vmb+roff;
            memcpy(out+rfile+roff, im->map+im->sliceOff+sg->fileoff, sg->filesize);
        }
        /* rewrite LC_SEGMENT_64.vmaddr + section addrs in the packed header */
        struct mh64*mh=(void*)(out+rx_file+im->rx_off);
        if(mh->magic!=MH_MAGIC_64){fprintf(stderr,"ABORT packed header %s\n",im->path);return 1;}
        struct lc*hc=(void*)((char*)mh+sizeof *mh);
        for(uint32_t ci=0;ci<mh->ncmds;ci++){ if(hc->cmd==LC_SEGMENT_64){ struct seg64*hs=(void*)hc; int ok=0;
            for(int s=0;s<im->nsegs;s++) if(!strncmp(di->segs[s].name,hs->segname,16)){
                int64_t delta=(int64_t)di->segs[s].vmaddr-(int64_t)hs->vmaddr;
                struct sect64*sc=(void*)((char*)hs+sizeof *hs);
                for(uint32_t si=0;si<hs->nsects;si++,sc++) sc->addr=(uint64_t)((int64_t)sc->addr+delta);
                hs->vmaddr=di->segs[s].vmaddr; ok=1; break; }
            if(!ok){fprintf(stderr,"ABORT header seg %.16s no table %s\n",hs->segname,im->path);return 1;} }
            hc=(void*)((char*)hc+hc->cmdsize); }
        /* seg-in-region invariant */
        for(int s=0;s<im->nsegs;s++){ struct dcc_seg*ds=&di->segs[s]; uint64_t rb=hdr.regions[ds->region_idx].vm_base,rs=hdr.regions[ds->region_idx].size;
            if(ds->vmaddr<rb||ds->vmaddr+ds->vmsize>rb+roundup(rs,DCC5_REGION_ALIGN)){fprintf(stderr,"ABORT seg outside region %s %.16s\n",im->path,ds->name);return 1;} }

        /* rewrite stored REBASE pointer VALUES in DATA (translate orig vmaddr -> region-relative vmaddr) */
        for(uint32_t fi=0;fi<g_nfix;fi++){ struct dcc_fixup*f=&g_fix[fi];
            if(f->image_index!=(uint32_t)i || f->kind!=DCC_FIX_REBASE) continue;
            uint64_t rfile = (f->loc_region==0)?rx_file : (f->loc_region==1)?rw_file : ro_file;
            uint64_t*loc=(uint64_t*)(out+rfile+f->loc_off);
            uint64_t orig=*loc; int found=0;
            for(int s=0;s<im->nsegs;s++){ struct src_seg*sg=&im->segs[s];
                if(orig>=sg->vmaddr && orig<sg->vmaddr+sg->vmsize){
                    *loc = di->segs[s].vmaddr + (orig - sg->vmaddr); found=1; break; } }
            if(!found){ fprintf(stderr,"ABORT rebase value 0x%llx in %s not within any segment\n",
                (unsigned long long)orig, im->path); return 1; }
        }
    }

    /* ===== perf#24f: RIP-relative TEXT rewrite pass (after headers+section addrs are packed) ===== */
    struct riprel_gate gate; memset(&gate,0,sizeof gate);
    uint64_t T_total=0,T_rw=0,T_st=0,T_pool=0,T_poolbytes=0;
    uint64_t S_total=0,S_rw=0,S_st=0,H_total=0,H_rw=0;
    for(int i=0;i<np;i++){ struct src_img*im=&imgs[i];
        uint8_t*rxblob=out+rx_file+im->rx_off;   /* packed __TEXT segment start */
        if(detect_pool(im)) return 1;
        T_poolbytes+=im->pool_count;
        if(rewrite_image_text(im,i,rxblob,RXb,RWb,ROb,&gate)) return 1;
        /* MANDATORY stub pass: llvm-objdump does NOT decode S_SYMBOL_STUBS, so the text pass above misses
         * them. Without this, every ff25 stub keeps its original TEXT->DATA disp32 => clang SIGSEGV. */
        if(rewrite_image_stubs(im,i,rxblob,RXb,RWb,ROb,&gate)) return 1;
        T_rw+=im->riprel_rewritten; T_st+=im->riprel_sametext; T_pool+=im->riprel_skipped_pool;
        S_total+=im->stub_total; S_rw+=im->stub_rewritten; S_st+=im->stub_sametext;
        H_total+=im->helper_total; H_rw+=im->helper_rewritten;
    }
    T_total=T_rw+T_st+T_pool;
    hdr.riprel_total=T_total; hdr.riprel_rewritten=T_rw; hdr.riprel_sametext=T_st; hdr.riprel_skipped_pool=T_pool;
    memcpy(out,&hdr,sizeof hdr);   /* refresh header totals */

    /* fixup ranges */
    { uint32_t cursor=0; for(int i=0;i<np;i++){ tbl[i].fixup_first=cursor; uint32_t cnt=0; while(cursor+cnt<g_nfix && g_fix[cursor+cnt].image_index==(uint32_t)i)cnt++; tbl[i].fixup_count=cnt; cursor+=cnt; } }

    memcpy(out+fixup_off, g_fix, fixup_bytes);
    if(g_extlen) memcpy(out+extern_off, g_extern, g_extlen);

    char tmp[700]; snprintf(tmp,sizeof tmp,"%s.tmp",outf);
    int fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd<0){perror("out");return 1;}
    if(write(fd,out,total)!=(ssize_t)total){perror("write");close(fd);unlink(tmp);return 1;}
    close(fd); if(rename(tmp,outf)){perror("rename");unlink(tmp);return 1;}

    fprintf(stderr,"OK DCC5: %d dylibs  RX=0x%llx RW=0x%llx RO=0x%llx total=0x%llx\n",np,
        (unsigned long long)g_rx,(unsigned long long)g_rw,(unsigned long long)g_ro,(unsigned long long)total);
    fprintf(stderr,"  fixups=%u  rebases=%d  binds=%d  [internal(ord)=%d self=%d flat-resolved=%d extern=%d]  extern-strtab=%uB\n",
        g_nfix,c_rebase,c_bind_internal+c_bind_self+c_bind_flat_resolved+c_bind_extern,
        c_bind_internal,c_bind_self,c_bind_flat_resolved,c_bind_extern,g_extlen);
    fprintf(stderr,"  UNRESOLVED (suspicious)=%d  lazy-unresolved-kept=%d\n",c_unresolved,c_lazy_rebase);
    fprintf(stderr,"  RIPREL: total=%llu rewritten(same-DATA/RO)=%llu same-TEXT(left)=%llu skipped-pool=%llu  [int32-overflow=%d outside-seg=%d pool-overlap=%d]\n",
        (unsigned long long)T_total,(unsigned long long)T_rw,(unsigned long long)T_st,(unsigned long long)T_pool,gate.int32_overflow,gate.target_class_bad,gate.pool_overlap);
    fprintf(stderr,"  constant-pool (data-in-text) bytes protected=%llu (never rewritten)\n",(unsigned long long)T_poolbytes);
    fprintf(stderr,"  STUBS (__stubs, ff25 — decoder-blind): total=%llu rewritten=%llu same-TEXT=%llu%s\n",
        (unsigned long long)S_total,(unsigned long long)S_rw,(unsigned long long)S_st, g_nostubs?"  [--no-stubs SKIPPED]":"");
    fprintf(stderr,"  STUB_HELPER (decoder-blind, lea-rip + jmp*): exec-bytes=%llu rip-rewritten=%llu%s\n",
        (unsigned long long)H_total,(unsigned long long)H_rw, g_nohelper?"  [--no-helper SKIPPED]":"");
    return 0;
}
