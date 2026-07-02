/* perf#24c2d-fix2 — DCC4 cache builder (UNIFIED ARENA, delta-exact layout).
 *
 * Fixes the perf#24c2d-callsite root cause: DCC2 region-packing separated each image's __TEXT and
 * __DATA, breaking baked x86-64 RIP-relative TEXT->DATA references (stubs/GOT). DCC4 lays every image
 * as a RIGID SLAB in one arena so each image's original intra-image segment spacing is preserved
 * EXACTLY; a single slide relocates all; no code is rewritten. Permissions are restored by a run-table
 * of mprotect runs after the reader applies fixups.
 *
 * Reuses the DCC2 builder's validated Mach-O parsing, export-trie resolver, and rebase/bind opcode
 * walkers. Only the LAYOUT (region-pack -> arena slab pack) and the FIXUP COORDINATE (region+offset ->
 * arena offset) change. Adds build-time gates: delta invariant, cross-perm page conflict == 0, and a
 * stubscan (every __stubs jmp*disp(rip) target lands in that image's own pointer sections).
 *
 * Deterministic (sorted list), atomic (tmp+rename), all-or-nothing.
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
#include <arpa/inet.h>
#include <errno.h>
#include "dcc4-format.h"

/* ---- minimal Mach-O ---- */
struct fat_header { uint32_t magic, nfat_arch; };
struct fat_arch   { uint32_t cputype, cpusubtype, offset, size, align; };
struct mh64 { uint32_t magic,cputype,cpusubtype,filetype,ncmds,sizeofcmds,flags,reserved; };
struct lc   { uint32_t cmd, cmdsize; };
struct seg64 { uint32_t cmd,cmdsize; char segname[16]; uint64_t vmaddr,vmsize,fileoff,filesize; uint32_t maxprot,initprot,nsects,flags; };
struct sect64 { char sectname[16]; char segname[16]; uint64_t addr,size; uint32_t offset,align,reloff,nreloc,flags,reserved1,reserved2,reserved3; };
struct dylib_command { uint32_t cmd,cmdsize,name_off,timestamp,cur,compat; };
struct dyld_info { uint32_t cmd,cmdsize,rebase_off,rebase_size,bind_off,bind_size,weak_bind_off,weak_bind_size,lazy_bind_off,lazy_bind_size,export_off,export_size; };
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
#define LC_DYLD_CHAINED_FIXUPS 0x80000034
#define SECTION_TYPE 0x000000ff
#define S_SYMBOL_STUBS 0x08

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
#define EXPORT_SYMBOL_FLAGS_REEXPORT 0x08

#define MAX_IMAGES 128
#define PTR 8

static uint64_t roundup(uint64_t v,uint64_t a){ return (v+a-1)&~(a-1); }
static uint64_t fnv1a(const void*p,size_t n,uint64_t h){ const uint8_t*b=p; for(size_t i=0;i<n;i++){h^=b[i];h*=1099511628211ULL;} return h; }
static uint64_t uleb(const uint8_t**p,const uint8_t*e){uint64_t r=0;int s=0;while(*p<e){uint8_t b=*(*p)++;r|=(uint64_t)(b&0x7f)<<s;if(!(b&0x80))break;s+=7;}return r;}
static int64_t sleb(const uint8_t**p,const uint8_t*e){int64_t r=0;int s=0;uint8_t b;do{b=*(*p)++;r|=(int64_t)(b&0x7f)<<s;s+=7;}while(b&0x80&&*p<e);if(s<64&&(b&0x40))r|=-(1LL<<s);return r;}

struct src_seg { char name[16]; uint64_t vmaddr,vmsize,fileoff,filesize; uint32_t prot; };
struct src_img {
    char path[256]; uint64_t sliceOff,sliceSize; uint8_t*map; uint64_t mapsize;
    uint64_t inode,mtime,size; uint8_t uuid[16];
    struct src_seg segs[DCC4_MAX_SEGS]; int nsegs;
    uint64_t textVmaddr;                 /* original TEXT vmaddr */
    uint64_t slab_base;                  /* arena offset of this image's TEXT (== image_arena_off) */
    uint64_t slab_span;                  /* image vm span (max seg end - textVmaddr), page rounded */
    struct dyld_info* di;
    char deps[64][128]; int ndeps;
    int dep_reexport[64];
    int has_chained;
};

/* arena offset of (segIndex, segOff-within-seg). DCC4: seg arena_off = slab_base + (seg.vmaddr - textVmaddr). */
static uint64_t seg_arena_base(struct src_img*im,int segIndex){ return im->slab_base + (im->segs[segIndex].vmaddr - im->textVmaddr); }
static int seg_to_arena(struct src_img*im,int segIndex,uint64_t segOff,uint64_t*aoff){
    if(segIndex<0||segIndex>=im->nsegs)return -1;
    *aoff = seg_arena_base(im,segIndex) + segOff; return 0;
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
            } else { uint64_t off=uleb(&q,trieEnd); *imgOff=off; return 1; }
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

static int parse_image(const char*hostpath, const char*guestpath, struct src_img*im){
    strncpy(im->path,guestpath,sizeof im->path-1);
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
            if(strcmp(s->segname,"__PAGEZERO")==0){ c=(void*)((char*)c+c->cmdsize); continue; }
            if(im->nsegs>=DCC4_MAX_SEGS){fprintf(stderr,"%s: too many segs\n",path);return -1;}
            struct src_seg*d=&im->segs[im->nsegs++];
            memcpy(d->name,s->segname,16); d->vmaddr=s->vmaddr; d->vmsize=s->vmsize;
            d->fileoff=s->fileoff; d->filesize=s->filesize; d->prot=s->initprot;
            if(!strcmp(s->segname,"__TEXT")) im->textVmaddr=s->vmaddr;
        } else if(c->cmd==LC_DYLD_INFO||c->cmd==LC_DYLD_INFO_ONLY){ im->di=(void*)c; }
        else if(c->cmd==LC_DYLD_CHAINED_FIXUPS){ im->has_chained=1; }
        else if(c->cmd==LC_LOAD_DYLIB||c->cmd==LC_LOAD_WEAK_DYLIB||c->cmd==LC_REEXPORT_DYLIB||c->cmd==LC_LOAD_UPWARD_DYLIB){
            struct dylib_command*dc=(void*)c; const char*nm=(char*)dc+dc->name_off; const char*l=strrchr(nm,'/'); l=l?l+1:nm;
            if(im->ndeps<64){ im->dep_reexport[im->ndeps]=(c->cmd==LC_REEXPORT_DYLIB); strncpy(im->deps[im->ndeps++],l,127); }
        } else if(c->cmd==LC_UUID){ struct uuid_cmd*u=(void*)c; memcpy(im->uuid,u->uuid,16); }
        c=(void*)((char*)c+c->cmdsize);
    }
    if(im->has_chained){fprintf(stderr,"%s: chained fixups not supported by DCC4\n",path);return -1;}
    /* slab span = max(seg.vmaddr+vmsize) - textVmaddr, page rounded */
    uint64_t maxend=0; for(int s=0;s<im->nsegs;s++){ uint64_t e=(im->segs[s].vmaddr-im->textVmaddr)+im->segs[s].vmsize; if(e>maxend)maxend=e; }
    im->slab_span=roundup(maxend,DCC4_PAGE);
    return 0;
}

/* globals */
static struct src_img*g_imgs; static int g_np;
static int find_img_by_leaf(const char*leaf){
    for(int i=0;i<g_np;i++){ const char*l=strrchr(g_imgs[i].path,'/'); l=l?l+1:g_imgs[i].path; if(!strcmp(l,leaf))return i; }
    return -1;
}
static int resolve_export(int imgIdx,const char*sym,int depth,uint64_t*aoff){
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
                if(imgOff>=sb && imgOff<se){ uint64_t inSeg=imgOff-sb; return seg_to_arena(im,s,inSeg,aoff)==0; }
            }
            return 0;
        } else if(r==2){
            if(rord>=1 && rord<=im->ndeps){ int tgt=find_img_by_leaf(im->deps[rord-1]); if(resolve_export(tgt,rname,depth+1,aoff))return 1; }
        }
    }
    for(int d=0;d<im->ndeps;d++){
        if(!im->dep_reexport[d]) continue;
        int tgt=find_img_by_leaf(im->deps[d]);
        if(tgt>=0 && resolve_export(tgt,sym,depth+1,aoff)) return 1;
    }
    return 0;
}
static int resolve_flat(const char*sym,uint64_t*aoff,int*whichImg){
    for(int i=0;i<g_np;i++){ if(resolve_export(i,sym,0,aoff)){ *whichImg=i; return 1; } }
    return 0;
}

/* ---- fixup emission (arena-offset coordinates) ---- */
struct dcc4_fixup* g_fix=NULL; uint32_t g_nfix=0,g_capfix=0;
char* g_extern=NULL; uint32_t g_extlen=0,g_extcap=0;
static uint32_t extern_add(const char*s){
    uint32_t need=strlen(s)+1;
    if(g_extlen+need>g_extcap){ g_extcap=(g_extcap?g_extcap*2:4096); while(g_extlen+need>g_extcap)g_extcap*=2; g_extern=realloc(g_extern,g_extcap); }
    uint32_t off=g_extlen; memcpy(g_extern+off,s,need); g_extlen+=need; return off;
}
static struct dcc4_fixup* fix_new(void){
    if(g_nfix>=g_capfix){ g_capfix=g_capfix?g_capfix*2:65536; g_fix=realloc(g_fix,g_capfix*sizeof *g_fix); }
    struct dcc4_fixup*f=&g_fix[g_nfix++]; memset(f,0,sizeof *f); return f;
}
static int c_rebase=0,c_bind_internal=0,c_bind_self=0,c_bind_flat_resolved=0,c_bind_extern=0,c_unresolved=0,c_lazy_rebase=0;

static void emit_rebase(struct src_img*im,int imgIdx,int segIndex,uint64_t segOff){
    uint64_t aoff;
    if(seg_to_arena(im,segIndex,segOff,&aoff)){ fprintf(stderr,"ABORT rebase seg map %s\n",im->path); exit(1);}
    struct dcc4_fixup*f=fix_new(); f->kind=DCC_FIX_REBASE; f->loc_off=aoff; f->image_index=imgIdx;
    c_rebase++;
}
static void emit_bind(struct src_img*im,int imgIdx,int segIndex,uint64_t segOff,const char*sym,int ord,int64_t addend,int isLazy){
    uint64_t aoff;
    if(seg_to_arena(im,segIndex,segOff,&aoff)){ fprintf(stderr,"ABORT bind seg map %s\n",im->path); exit(1);}
    struct dcc4_fixup*f=fix_new(); f->loc_off=aoff; f->addend=addend; f->image_index=imgIdx;
    uint64_t troff; int done=0;
    if(ord>=1){ if(ord<=im->ndeps){ int tgt=find_img_by_leaf(im->deps[ord-1]); if(resolve_export(tgt,sym,0,&troff)){ f->kind=DCC_FIX_BIND_INTERNAL; f->tgt_off=troff; c_bind_internal++; done=1; } } }
    else if(ord==0){ if(resolve_export(imgIdx,sym,0,&troff)){ f->kind=DCC_FIX_BIND_INTERNAL; f->tgt_off=troff; c_bind_self++; done=1; } }
    else if(ord==-2){ int w; if(resolve_flat(sym,&troff,&w)){ f->kind=DCC_FIX_BIND_INTERNAL; f->tgt_off=troff; c_bind_flat_resolved++; done=1; } }
    if(!done){
        if(isLazy){ f->kind=DCC_FIX_BIND_EXTERN_LAZY; f->extern_sym=extern_add(sym); c_lazy_rebase++; return; }
        f->kind=DCC_FIX_BIND_EXTERN; f->extern_sym=extern_add(sym); c_bind_extern++;
        if(ord>=1 || ord==0){ c_unresolved++;
            if(getenv("DCC4_DEBUG")){ const char*dep=(ord>=1&&ord<=im->ndeps)?im->deps[ord-1]:"<self>";
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

/* ---- DCC4 layout: greedy page-perm slab packer ---- */
/* per-page permission map of the whole arena. index = arena page. 0 = free. */
static uint8_t* g_pageperm=NULL; static uint64_t g_npages=0, g_cappages=0;
static void ensure_pages(uint64_t upto){ if(upto<=g_cappages)return; uint64_t nc=g_cappages?g_cappages*2:4096; while(nc<upto)nc*=2; g_pageperm=realloc(g_pageperm,nc); memset(g_pageperm+g_cappages,0,nc-g_cappages); g_cappages=nc; }
/* try to place image slab at arena page 'basepg'; return 1 only if EVERY page it needs is FREE.
 * Two different images must NEVER share bytes (even same-perm) — they hold different content.
 * Same-perm RUN MERGING happens naturally when slabs ABUT (adjacent pages), not by overlap. */
static int slab_fits(struct src_img*im,uint64_t basepg){
    for(int s=0;s<im->nsegs;s++){
        uint64_t rel=im->segs[s].vmaddr-im->textVmaddr;
        uint64_t startpg=basepg+rel/DCC4_PAGE;
        uint64_t npg=roundup(im->segs[s].vmsize,DCC4_PAGE)/DCC4_PAGE;
        ensure_pages(startpg+npg);
        for(uint64_t p=startpg;p<startpg+npg;p++) if(g_pageperm[p]) return 0;
    }
    return 1;
}
static void slab_commit(struct src_img*im,uint64_t basepg){
    for(int s=0;s<im->nsegs;s++){
        uint64_t rel=im->segs[s].vmaddr-im->textVmaddr;
        uint64_t startpg=basepg+rel/DCC4_PAGE;
        uint64_t npg=roundup(im->segs[s].vmsize,DCC4_PAGE)/DCC4_PAGE;
        uint8_t pr=(uint8_t)im->segs[s].prot;
        ensure_pages(startpg+npg);
        for(uint64_t p=startpg;p<startpg+npg;p++) g_pageperm[p]=pr;
        if(startpg+npg>g_npages) g_npages=startpg+npg;
    }
    im->slab_base = basepg*DCC4_PAGE;
}

int main(int argc,char**argv){
    if(argc<4){fprintf(stderr,"usage: %s <install_root> <closure_list.txt> <out.dcc4>\n",argv[0]);return 2;}
    const char*root=argv[1]; const char*listf=argv[2]; const char*outf=argv[3];

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

    /* LAYOUT: greedy first-fit, largest-slab-first (deterministic tie-break by path). */
    int order[MAX_IMAGES]; for(int i=0;i<np;i++)order[i]=i;
    for(int a=0;a<np;a++)for(int b=a+1;b<np;b++){
        struct src_img*A=&imgs[order[a]],*B=&imgs[order[b]];
        if(B->slab_span>A->slab_span || (B->slab_span==A->slab_span && strcmp(B->path,A->path)<0)){ int t=order[a];order[a]=order[b];order[b]=t; }
    }
    for(int oi=0;oi<np;oi++){ struct src_img*im=&imgs[order[oi]];
        uint64_t basepg=0; while(!slab_fits(im,basepg)) basepg++;
        slab_commit(im,basepg);
    }
    uint64_t arena_size=roundup(g_npages*DCC4_PAGE,DCC4_ARENA_ALIGN);

    /* generate fixups (needs slab_base assigned) */
    for(int i=0;i<np;i++){ walk_rebases(&imgs[i],i); walk_binds(&imgs[i],i); }

    /* build run table from the page-perm map (contiguous same-perm runs) */
    struct dcc4_run runs[DCC4_MAX_RUNS]; uint32_t nruns=0;
    { uint64_t p=0; while(p<g_npages){ if(!g_pageperm[p]){p++;continue;} uint8_t pr=g_pageperm[p]; uint64_t start=p; while(p<g_npages && g_pageperm[p]==pr) p++;
        if(nruns>=DCC4_MAX_RUNS){fprintf(stderr,"ABORT too many perm runs\n");return 1;}
        runs[nruns].arena_off=start*DCC4_PAGE; runs[nruns].size=(p-start)*DCC4_PAGE; runs[nruns].prot=pr; runs[nruns]._pad=0; nruns++; } }

    /* file layout */
    struct dcc4_header hdr; memset(&hdr,0,sizeof hdr);
    hdr.magic=DCC4_MAGIC; hdr.version=DCC4_VERSION; hdr.arch=CPU_TYPE_X86_64; hdr.image_count=np;
    hdr.closure_hash=chash; strncpy(hdr.install_root,root,sizeof hdr.install_root-1);
    uint64_t tbl_off=sizeof(struct dcc4_header);
    uint64_t images_bytes=(uint64_t)np*sizeof(struct dcc4_image);
    uint64_t run_off=tbl_off+images_bytes;
    uint64_t run_bytes=(uint64_t)nruns*sizeof(struct dcc4_run);
    uint64_t fixup_off=run_off+run_bytes;
    uint64_t fixup_bytes=(uint64_t)g_nfix*sizeof(struct dcc4_fixup);
    uint64_t extern_off=fixup_off+fixup_bytes;
    uint64_t extern_bytes=g_extlen;
    uint64_t arena_file=roundup(extern_off+extern_bytes,DCC4_ARENA_ALIGN);
    uint64_t total=roundup(arena_file+arena_size,DCC4_ARENA_ALIGN);

    hdr.arena_size=arena_size; hdr.arena_file_off=arena_file;
    hdr.run_count=nruns; hdr.run_off=run_off;
    hdr.fixup_off=fixup_off; hdr.fixup_count=g_nfix;
    hdr.extern_off=extern_off; hdr.extern_size=g_extlen;

    uint8_t*out=calloc(1,total); memcpy(out,&hdr,sizeof hdr);
    memcpy(out+run_off,runs,run_bytes);
    struct dcc4_image*tbl=(void*)(out+tbl_off);

    for(int i=0;i<np;i++){ struct src_img*im=&imgs[i]; struct dcc4_image*di=&tbl[i];
        strncpy(di->path,im->path,sizeof di->path-1); memcpy(di->uuid,im->uuid,16);
        di->src_inode=im->inode; di->src_mtime=im->mtime; di->src_size=im->size;
        di->image_arena_off=im->slab_base; di->nsegs=im->nsegs; di->init_index=i;
        for(int s=0;s<im->nsegs;s++){ struct src_seg*sg=&im->segs[s]; struct dcc4_seg*ds=&di->segs[s];
            memcpy(ds->name,sg->name,16); ds->vmsize=sg->vmsize; ds->filesize=sg->filesize; ds->prot=sg->prot;
            ds->arena_off = seg_arena_base(im,s);
            memcpy(out+arena_file+ds->arena_off, im->map+im->sliceOff+sg->fileoff, sg->filesize);
        }
        /* rewrite LC_SEGMENT_64.vmaddr + section_64.addr in the packed header to arena-absolute
         * (arena_file mapped at arena base => vmaddr == arena_off; getSlide()=arena). */
        struct mh64*mh=(void*)(out+arena_file+im->slab_base);
        if(mh->magic!=MH_MAGIC_64){fprintf(stderr,"ABORT packed header %s\n",im->path);return 1;}
        struct lc*hc=(void*)((char*)mh+sizeof *mh);
        for(uint32_t ci=0;ci<mh->ncmds;ci++){ if(hc->cmd==LC_SEGMENT_64){ struct seg64*hs=(void*)hc;
            if(strcmp(hs->segname,"__PAGEZERO")==0){ hc=(void*)((char*)hc+hc->cmdsize); continue; }
            int ok=0;
            for(int s=0;s<im->nsegs;s++) if(!strncmp(di->segs[s].name,hs->segname,16)){
                int64_t delta=(int64_t)di->segs[s].arena_off-(int64_t)hs->vmaddr;
                struct sect64*sc=(void*)((char*)hs+sizeof *hs);
                for(uint32_t si=0;si<hs->nsects;si++,sc++) sc->addr=(uint64_t)((int64_t)sc->addr+delta);
                hs->vmaddr=di->segs[s].arena_off; ok=1; break; }
            if(!ok){fprintf(stderr,"ABORT header seg %.16s no table %s\n",hs->segname,im->path);return 1;} }
            hc=(void*)((char*)hc+hc->cmdsize); }

        /* translate stored REBASE VALUES: orig same-image vmaddr -> arena_off of containing seg
         * (reader does *loc += slide=arena; so stored must be region-relative arena offset). */
        for(uint32_t fi=0;fi<g_nfix;fi++){ struct dcc4_fixup*f=&g_fix[fi];
            if(f->image_index!=(uint32_t)i || f->kind!=DCC_FIX_REBASE) continue;
            uint64_t*loc=(uint64_t*)(out+arena_file+f->loc_off);
            uint64_t orig=*loc; int found=0;
            for(int s=0;s<im->nsegs;s++){ struct src_seg*sg=&im->segs[s];
                if(orig>=sg->vmaddr && orig<sg->vmaddr+sg->vmsize){
                    *loc = seg_arena_base(im,s) + (orig - sg->vmaddr); found=1; break; } }
            if(!found){ fprintf(stderr,"ABORT rebase value 0x%llx in %s not within any segment\n",
                (unsigned long long)orig, im->path); return 1; }
        }
    }

    /* per-image fixup ranges (emitted grouped per image, in order) */
    { uint32_t cursor=0; for(int i=0;i<np;i++){ tbl[i].fixup_first=cursor; uint32_t cnt=0; while(cursor+cnt<g_nfix && g_fix[cursor+cnt].image_index==(uint32_t)i)cnt++; tbl[i].fixup_count=cnt; cursor+=cnt; } }

    memcpy(out+fixup_off, g_fix, fixup_bytes);
    if(g_extlen) memcpy(out+extern_off, g_extern, g_extlen);

    /* ---- BUILD-TIME HARD GATES ---- */
    int gate_fail=0;
    /* (1) delta invariant: every image (arena DATA - arena TEXT) == (orig DATA - orig TEXT) */
    for(int i=0;i<np;i++){ struct src_img*im=&imgs[i];
        int ti=-1,didx=-1; for(int s=0;s<im->nsegs;s++){ if(!strcmp(im->segs[s].name,"__TEXT"))ti=s; if(!strcmp(im->segs[s].name,"__DATA"))didx=s; }
        if(ti<0)continue; if(didx<0)continue;
        int64_t arenaDelta=(int64_t)seg_arena_base(im,didx)-(int64_t)seg_arena_base(im,ti);
        int64_t origDelta=(int64_t)im->segs[didx].vmaddr-(int64_t)im->segs[ti].vmaddr;
        if(arenaDelta!=origDelta){ fprintf(stderr,"GATE-FAIL delta %s arena=0x%llx orig=0x%llx\n",im->path,(unsigned long long)arenaDelta,(unsigned long long)origDelta); gate_fail=1; }
    }
    /* (2) all fixup loc_off must land in a RW run (writable) */
    for(uint32_t fi=0;fi<g_nfix;fi++){ uint64_t lo=g_fix[fi].loc_off; int inrw=0;
        for(uint32_t r=0;r<nruns;r++) if(runs[r].prot==DCC4_PROT_RW && lo>=runs[r].arena_off && lo+8<=runs[r].arena_off+runs[r].size){inrw=1;break;}
        if(!inrw){ fprintf(stderr,"GATE-FAIL fixup %u loc 0x%llx not in a RW run\n",fi,(unsigned long long)lo); gate_fail=1; if(fi>20)break; }
    }
    /* (3) STUBSCAN: every __stubs `ff 25 disp32` target lands in that image's __la/__got/__nl section */
    { int scanned=0,bad=0;
      for(int i=0;i<np;i++){ struct src_img*im=&imgs[i];
        struct mh64*mh=(void*)(out+arena_file+im->slab_base);
        struct lc*hc=(void*)((char*)mh+sizeof *mh);
        /* collect pointer-section arena ranges + find __stubs */
        uint64_t ptr_lo[16],ptr_hi[16]; int nptr=0; uint64_t stubs_ao=0,stubs_sz=0;
        for(uint32_t ci=0;ci<mh->ncmds;ci++){ if(hc->cmd==LC_SEGMENT_64){ struct seg64*hs=(void*)hc;
            struct sect64*sc=(void*)((char*)hs+sizeof *hs);
            for(uint32_t si=0;si<hs->nsects;si++,sc++){
                if(!strncmp(sc->sectname,"__la_symbol_ptr",16)||!strncmp(sc->sectname,"__got",16)||!strncmp(sc->sectname,"__nl_symbol_ptr",16)){
                    if(nptr<16){ ptr_lo[nptr]=sc->addr; ptr_hi[nptr]=sc->addr+sc->size; nptr++; } }
                if((sc->flags&SECTION_TYPE)==S_SYMBOL_STUBS || !strncmp(sc->sectname,"__stubs",16)){ stubs_ao=sc->addr; stubs_sz=sc->size; }
            } }
            hc=(void*)((char*)hc+hc->cmdsize); }
        if(!stubs_sz||!nptr) continue;
        uint8_t*sbytes=out+arena_file+stubs_ao;
        for(uint64_t o=0;o+6<=stubs_sz;){
            if(sbytes[o]==0xff && sbytes[o+1]==0x25){
                int32_t disp; memcpy(&disp,sbytes+o+2,4);
                uint64_t tgt=(stubs_ao+o+6)+(int64_t)disp;
                int ok=0; for(int k=0;k<nptr;k++) if(tgt>=ptr_lo[k]&&tgt<ptr_hi[k]){ok=1;break;}
                scanned++; if(!ok){ bad++; if(bad<=5) fprintf(stderr,"GATE-FAIL stub %s @0x%llx -> 0x%llx not in ptr section\n",im->path,(unsigned long long)(stubs_ao+o),(unsigned long long)tgt); }
                o+=6;
            } else o++;
        }
      }
      fprintf(stderr,"  stubscan: scanned=%d out-of-ptr-section=%d\n",scanned,bad);
      if(bad) gate_fail=1;
    }
    if(gate_fail){ fprintf(stderr,"ABORT: build-time gate(s) failed\n"); return 1; }

    char tmp[700]; snprintf(tmp,sizeof tmp,"%s.tmp",outf);
    int fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd<0){perror("out");return 1;}
    if(write(fd,out,total)!=(ssize_t)total){perror("write");close(fd);unlink(tmp);return 1;}
    close(fd); if(rename(tmp,outf)){perror("rename");unlink(tmp);return 1;}

    uint32_t rx=0,rw=0,ro=0,rxpg=0,rwpg=0,ropg=0;
    for(uint32_t r=0;r<nruns;r++){ if(runs[r].prot==DCC4_PROT_RX){rx++;rxpg+=runs[r].size/DCC4_PAGE;} else if(runs[r].prot==DCC4_PROT_RW){rw++;rwpg+=runs[r].size/DCC4_PAGE;} else {ro++;ropg+=runs[r].size/DCC4_PAGE;} }
    fprintf(stderr,"OK DCC4: %d dylibs  arena=0x%llx (%lluKB)  runs=%u (RX=%u RW=%u RO=%u) => %u VMAs\n",np,
        (unsigned long long)arena_size,(unsigned long long)arena_size/1024,nruns,rx,rw,ro,nruns);
    fprintf(stderr,"  fixups=%u  rebases=%d  binds=%d [internal=%d self=%d flat=%d extern=%d lazy-sentinel=%d]  extern-strtab=%uB\n",
        g_nfix,c_rebase,c_bind_internal+c_bind_self+c_bind_flat_resolved+c_bind_extern,
        c_bind_internal,c_bind_self,c_bind_flat_resolved,c_bind_extern,c_lazy_rebase,g_extlen);
    fprintf(stderr,"  GATES PASS: delta-invariant, fixups-in-RW, stubscan.  UNRESOLVED(suspicious)=%d\n",c_unresolved);
    return 0;
}
