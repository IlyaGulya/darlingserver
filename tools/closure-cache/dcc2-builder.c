/* perf#24c2b — DCC2 cache builder (region-packed + precomputed cache-native fixup table).
 *
 * Standalone host tool, NO dyld dependency. Extends the perf#24c1 builder:
 *   - 3 per-permission regions (RX/RW/RO), each image's LC_SEGMENT_64.vmaddr rewritten in the packed
 *     header so getSlide() reproduces every seg addr under one uniform slide (perf#24c2a fix).
 *   - PLUS a cache-native fixup table (DCC_REBASE / DCC_BIND_INTERNAL / DCC_BIND_EXTERN) so the reader
 *     can skip the normal Mach-O fixup engine (known-wrong for region-packed layout).
 *
 * Bind resolution (build time): every two-level (ordinal>=1) bind's symbol is resolved via the target
 * dylib's export trie into a cache-relative (region,offset); self (ordinal 0) resolved in same image;
 * flat (ordinal -2) resolved against the whole closure set if a definer is present, else recorded as an
 * EXTERN (runtime dyld symbol resolver). Nothing is silently dropped.
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
#include <arpa/inet.h>
#include <errno.h>
#include "dcc2-format.h"

/* ---- minimal Mach-O ---- */
struct fat_header { uint32_t magic, nfat_arch; };
struct fat_arch   { uint32_t cputype, cpusubtype, offset, size, align; };
struct mh64 { uint32_t magic,cputype,cpusubtype,filetype,ncmds,sizeofcmds,flags,reserved; };
struct lc   { uint32_t cmd, cmdsize; };
struct seg64 { uint32_t cmd,cmdsize; char segname[16]; uint64_t vmaddr,vmsize,fileoff,filesize; uint32_t maxprot,initprot,nsects,flags; };
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
struct src_img {
    char path[256]; uint64_t sliceOff,sliceSize; uint8_t*map; uint64_t mapsize;
    uint64_t inode,mtime,size; uint8_t uuid[16];
    struct src_seg segs[DCC2_MAX_SEGS]; int nsegs;
    uint64_t textVmaddr;                 /* original TEXT vmaddr */
    uint64_t rx_off, rw_off, ro_off;     /* region offsets for TEXT/DATA/LINKEDIT */
    struct dyld_info* di;                /* points into map (slice-relative resolved) */
    char deps[64][128]; int ndeps;       /* LC_LOAD_DYLIB leaf names, ordinal = index+1 */
    int dep_reexport[64];                /* 1 if the dep is LC_REEXPORT_DYLIB */
    int has_chained;
};

static uint64_t g_rx=0,g_rw=0,g_ro=0;

/* map original (segIndex,segOffset) of an image to a cache (region,region_off).
 * segOffset is the offset within that segment (== vmaddr-seg.vmaddr == fileoff-seg.fileoff for our dylibs). */
static int seg_to_cache(struct src_img*im,int segIndex,uint64_t segOff,int*region,uint64_t*roff){
    if(segIndex<0||segIndex>=im->nsegs)return -1;
    struct src_seg*sg=&im->segs[segIndex];
    uint64_t base;
    if(sg->region==0)base=im->rx_off; else if(sg->region==1)base=im->rw_off; else base=im->ro_off;
    *region=sg->region; *roff=base+segOff; return 0;
}

/* find which segment a given image-relative vmoffset falls into (for rebase/bind SET_SEGMENT gives index directly,
 * but we also need seg base to compute offset-in-seg). */
static uint64_t seg_vmbase_orig(struct src_img*im,int segIndex){ return im->segs[segIndex].vmaddr; }

/* ---- export trie walk: resolve a symbol name -> image offset (from image base) ---- */
/* returns 1 and sets *imgOff if found (regular export); if re-export, sets *reexportOrdinal (>0) and *reexportName. */
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

static int parse_image(const char*hostpath, const char*guestpath, struct src_img*im){
    /* perf#24c2e: store the GUEST path (vchroot-relative, e.g. /usr/lib/system/foo.dylib) — that is
     * what the dyld2 reader stats/opens inside the guest. Open the HOST path for build-time I/O. */
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
            if(im->nsegs>=DCC2_MAX_SEGS){fprintf(stderr,"%s: too many segs\n",path);return -1;}
            struct src_seg*d=&im->segs[im->nsegs++];
            memcpy(d->name,s->segname,16); d->vmaddr=s->vmaddr; d->vmsize=s->vmsize;
            d->fileoff=s->fileoff; d->filesize=s->filesize; d->prot=s->initprot;
            if(!strcmp(s->segname,"__TEXT")){d->region=0; im->textVmaddr=s->vmaddr;}
            else if(!strcmp(s->segname,"__LINKEDIT")) d->region=2; else d->region=1;
        } else if(c->cmd==LC_DYLD_INFO||c->cmd==LC_DYLD_INFO_ONLY){ im->di=(void*)c; }
        else if(c->cmd==LC_DYLD_CHAINED_FIXUPS){ im->has_chained=1; }
        else if(c->cmd==LC_LOAD_DYLIB||c->cmd==LC_LOAD_WEAK_DYLIB||c->cmd==LC_REEXPORT_DYLIB||c->cmd==LC_LOAD_UPWARD_DYLIB){
            struct dylib_command*dc=(void*)c; const char*nm=(char*)dc+dc->name_off; const char*l=strrchr(nm,'/'); l=l?l+1:nm;
            if(im->ndeps<64){ im->dep_reexport[im->ndeps]=(c->cmd==LC_REEXPORT_DYLIB); strncpy(im->deps[im->ndeps++],l,127); }
        } else if(c->cmd==LC_UUID){ struct uuid_cmd*u=(void*)c; memcpy(im->uuid,u->uuid,16); }
        c=(void*)((char*)c+c->cmdsize);
    }
    if(im->has_chained){fprintf(stderr,"%s: chained fixups not supported by DCC2 (audit says closure is 100%% opcode)\n",path);return -1;}
    return 0;
}

/* globals for the whole set (for bind resolution) */
static struct src_img*g_imgs; static int g_np;
/* map leaf name -> image index */
static int find_img_by_leaf(const char*leaf){
    for(int i=0;i<g_np;i++){ const char*l=strrchr(g_imgs[i].path,'/'); l=l?l+1:g_imgs[i].path; if(!strcmp(l,leaf))return i; }
    return -1;
}
/* resolve a symbol exported by image idx -> region/roff. follows re-exports transitively. returns 1/0. */
static int resolve_export(int imgIdx,const char*sym,int depth,int*region,uint64_t*roff){
    if(imgIdx<0||depth>16) return 0;
    struct src_img*im=&g_imgs[imgIdx];
    if(im->di && im->di->export_size){
        const uint8_t*ts=im->map+im->sliceOff+im->di->export_off;
        const uint8_t*te=ts+im->di->export_size;
        uint64_t imgOff=0; int rord=0; const char*rname=NULL;
        int r=trie_find(ts,te,sym,&imgOff,&rord,&rname);
        if(r==1){
            /* imgOff is offset from image base (== vmaddr - textVmaddr). find seg. */
            for(int s=0;s<im->nsegs;s++){
                uint64_t sb=im->segs[s].vmaddr - im->textVmaddr;   /* seg base as image offset */
                uint64_t se=sb+im->segs[s].vmsize;
                if(imgOff>=sb && imgOff<se){ uint64_t inSeg=imgOff-sb; return seg_to_cache(im,s,inSeg,region,roff)==0; }
            }
            return 0;
        } else if(r==2){
            /* explicit re-export trie node: rord is dep ordinal in THIS image; rname is the name in the target */
            if(rord>=1 && rord<=im->ndeps){ int tgt=find_img_by_leaf(im->deps[rord-1]); if(resolve_export(tgt,rname,depth+1,region,roff))return 1; }
        }
    }
    /* not in this image's trie: follow LC_REEXPORT_DYLIB deps (umbrella libraries like libSystem.B
     * re-export their sub-dylibs' whole export set implicitly, not via explicit trie nodes). */
    for(int d=0;d<im->ndeps;d++){
        if(!im->dep_reexport[d]) continue;
        int tgt=find_img_by_leaf(im->deps[d]);
        if(tgt>=0 && resolve_export(tgt,sym,depth+1,region,roff)) return 1;
    }
    return 0;
}
/* flat resolution: search all images' export tries */
static int resolve_flat(const char*sym,int*region,uint64_t*roff,int*whichImg){
    for(int i=0;i<g_np;i++){ if(resolve_export(i,sym,0,region,roff)){ *whichImg=i; return 1; } }
    return 0;
}

/* ---- fixup emission ---- */
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

/* counters for the gate */
static int c_rebase=0,c_bind_internal=0,c_bind_self=0,c_bind_flat_resolved=0,c_bind_extern=0,c_unresolved=0;

static void emit_rebase(struct src_img*im,int imgIdx,int segIndex,uint64_t segOff){
    int reg; uint64_t roff;
    if(seg_to_cache(im,segIndex,segOff,&reg,&roff)){ fprintf(stderr,"ABORT rebase seg map %s\n",im->path); exit(1);}
    struct dcc_fixup*f=fix_new(); f->kind=DCC_FIX_REBASE; f->loc_region=reg; f->loc_off=roff; f->image_index=imgIdx;
    c_rebase++;
}
static void emit_bind(struct src_img*im,int imgIdx,int segIndex,uint64_t segOff,const char*sym,int ord,int64_t addend){
    int reg; uint64_t roff;
    if(seg_to_cache(im,segIndex,segOff,&reg,&roff)){ fprintf(stderr,"ABORT bind seg map %s\n",im->path); exit(1);}
    struct dcc_fixup*f=fix_new(); f->loc_region=reg; f->loc_off=roff; f->addend=addend; f->image_index=imgIdx;
    int tr; uint64_t troff; int done=0;
    if(ord>=1){ /* two-level: dep ordinal */
        if(ord<=im->ndeps){ int tgt=find_img_by_leaf(im->deps[ord-1]); if(resolve_export(tgt,sym,0,&tr,&troff)){ f->kind=DCC_FIX_BIND_INTERNAL; f->tgt_region=tr; f->tgt_off=troff; c_bind_internal++; done=1; } }
    } else if(ord==0){ /* self */
        if(resolve_export(imgIdx,sym,0,&tr,&troff)){ f->kind=DCC_FIX_BIND_INTERNAL; f->tgt_region=tr; f->tgt_off=troff; c_bind_self++; done=1; }
    } else if(ord==-2){ /* flat */
        int w; if(resolve_flat(sym,&tr,&troff,&w)){ f->kind=DCC_FIX_BIND_INTERNAL; f->tgt_region=tr; f->tgt_off=troff; c_bind_flat_resolved++; done=1; }
    }
    if(!done){ /* extern: leave for runtime dyld resolver, record symbol */
        f->kind=DCC_FIX_BIND_EXTERN; f->extern_sym=extern_add(sym); c_bind_extern++;
        if(ord>=1 || ord==0){ c_unresolved++;  /* two-level/self that we FAILED to resolve = suspicious */
            if(getenv("DCC2_DEBUG")){ const char*dep=(ord>=1&&ord<=im->ndeps)?im->deps[ord-1]:"<self>";
                fprintf(stderr,"  UNRESOLVED %s ord=%d(%s) sym=%s\n", strrchr(im->path,'/')+1, ord, dep, sym); } }
    }
}

/* walk rebase opcode stream, emitting DCC_REBASE per rebased pointer */
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
/* walk bind opcode stream */
static void walk_binds(struct src_img*im,int imgIdx){
    if(!im->di||!im->di->bind_size)return;
    const uint8_t*p=im->map+im->sliceOff+im->di->bind_off, *e=p+im->di->bind_size;
    int segIndex=0; uint64_t segOff=0; int ord=0; const char*sym=""; int64_t addend=0;
    while(p<e){ uint8_t op=*p++; uint8_t im4=op&0x0f; op&=0xf0;
        switch(op){
            case BIND_OP_DONE: return;
            case BIND_OP_SET_DYLIB_ORDINAL_IMM: ord=im4; break;
            case BIND_OP_SET_DYLIB_ORDINAL_ULEB: ord=(int)uleb(&p,e); break;
            case BIND_OP_SET_DYLIB_SPECIAL_IMM: ord=(im4==0)?0:(int8_t)(im4|0xf0); break;
            case BIND_OP_SET_SYMBOL_TRAILING_FLAGS_IMM: sym=(const char*)p; while(*p)p++; p++; break;
            case BIND_OP_SET_TYPE_IMM: break;
            case BIND_OP_SET_ADDEND_SLEB: addend=sleb(&p,e); break;
            case BIND_OP_SET_SEGMENT_AND_OFFSET_ULEB: segIndex=im4; segOff=uleb(&p,e); break;
            case BIND_OP_ADD_ADDR_ULEB: segOff+=uleb(&p,e); break;
            case BIND_OP_DO_BIND: emit_bind(im,imgIdx,segIndex,segOff,sym,ord,addend); segOff+=PTR; break;
            case BIND_OP_DO_BIND_ADD_ADDR_ULEB: emit_bind(im,imgIdx,segIndex,segOff,sym,ord,addend); segOff+=PTR+uleb(&p,e); break;
            case BIND_OP_DO_BIND_ADD_ADDR_IMM_SCALED: emit_bind(im,imgIdx,segIndex,segOff,sym,ord,addend); segOff+=PTR+(uint64_t)im4*PTR; break;
            case BIND_OP_DO_BIND_ULEB_TIMES_SKIPPING_ULEB: { uint64_t cnt=uleb(&p,e); uint64_t skip=uleb(&p,e); for(uint64_t i=0;i<cnt;i++){ emit_bind(im,imgIdx,segIndex,segOff,sym,ord,addend); segOff+=PTR+skip; } } break;
            default: fprintf(stderr,"ABORT unknown bind op 0x%x in %s\n",op,im->path); exit(1);
        }
    }
}

int main(int argc,char**argv){
    if(argc<4){fprintf(stderr,"usage: %s <install_root> <closure_list.txt> <out.dcc2>\n",argv[0]);return 2;}
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

    /* region offsets (first-fit, page-aligned per seg) */
    for(int i=0;i<np;i++){ struct src_img*im=&imgs[i];
        for(int s=0;s<im->nsegs;s++){ uint64_t sz=roundup(im->segs[s].vmsize,DCC2_REGION_ALIGN);
            if(im->segs[s].region==0){im->rx_off=g_rx;g_rx+=sz;} else if(im->segs[s].region==1){im->rw_off=g_rw;g_rw+=sz;} else {im->ro_off=g_ro;g_ro+=sz;} }
    }
    uint64_t RXb=0, RWb=roundup(g_rx,DCC2_REGION_ALIGN), ROb=roundup(RWb+g_rw,DCC2_REGION_ALIGN);

    /* generate the fixup table (needs region offsets assigned above) */
    for(int i=0;i<np;i++){ walk_rebases(&imgs[i],i); walk_binds(&imgs[i],i); }

    /* layout: header, image_table, fixups, extern strtab, region blobs */
    struct dcc_header hdr; memset(&hdr,0,sizeof hdr);
    hdr.magic=DCC2_MAGIC; hdr.version=DCC2_VERSION; hdr.arch=CPU_TYPE_X86_64; hdr.image_count=np;
    hdr.closure_hash=chash; strncpy(hdr.install_root,root,sizeof hdr.install_root-1);

    uint64_t tbl_off=sizeof(struct dcc_header);
    uint64_t images_bytes=(uint64_t)np*sizeof(struct dcc_image);
    uint64_t fixup_off=tbl_off+images_bytes;
    uint64_t fixup_bytes=(uint64_t)g_nfix*sizeof(struct dcc_fixup);
    uint64_t extern_off=fixup_off+fixup_bytes;
    uint64_t extern_bytes=g_extlen;
    uint64_t rx_file=roundup(extern_off+extern_bytes,DCC2_REGION_ALIGN);
    uint64_t rw_file=roundup(rx_file+g_rx,DCC2_REGION_ALIGN);
    uint64_t ro_file=roundup(rw_file+g_rw,DCC2_REGION_ALIGN);
    uint64_t total=roundup(ro_file+g_ro,DCC2_REGION_ALIGN);

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
            uint64_t roff,vmb,rfile; int ridx=sg->region;
            if(ridx==0){roff=im->rx_off;vmb=RXb;rfile=rx_file;} else if(ridx==1){roff=im->rw_off;vmb=RWb;rfile=rw_file;} else {roff=im->ro_off;vmb=ROb;rfile=ro_file;}
            ds->region_idx=ridx; ds->region_off=roff; ds->vmaddr=vmb+roff;
            memcpy(out+rfile+roff, im->map+im->sliceOff+sg->fileoff, sg->filesize);
        }
        /* rewrite LC_SEGMENT_64.vmaddr in the packed header (perf#24c2a fix) */
        struct mh64*mh=(void*)(out+rx_file+im->rx_off);
        if(mh->magic!=MH_MAGIC_64){fprintf(stderr,"ABORT packed header %s\n",im->path);return 1;}
        struct lc*hc=(void*)((char*)mh+sizeof *mh);
        for(uint32_t ci=0;ci<mh->ncmds;ci++){ if(hc->cmd==LC_SEGMENT_64){ struct seg64*hs=(void*)hc; int ok=0;
            for(int s=0;s<im->nsegs;s++) if(!strncmp(di->segs[s].name,hs->segname,16)){ hs->vmaddr=di->segs[s].vmaddr; ok=1; break; }
            if(!ok){fprintf(stderr,"ABORT header seg %.16s no table %s\n",hs->segname,im->path);return 1;} }
            hc=(void*)((char*)hc+hc->cmdsize); }
        /* seg-in-region invariant */
        for(int s=0;s<im->nsegs;s++){ struct dcc_seg*ds=&di->segs[s]; uint64_t rb=hdr.regions[ds->region_idx].vm_base,rs=hdr.regions[ds->region_idx].size;
            if(ds->vmaddr<rb||ds->vmaddr+ds->vmsize>rb+roundup(rs,DCC2_REGION_ALIGN)){fprintf(stderr,"ABORT seg outside region %s %.16s\n",im->path,ds->name);return 1;} }
    }

    /* assign per-image fixup ranges (fixups were emitted grouped per image, in order) */
    { uint32_t cursor=0; for(int i=0;i<np;i++){ tbl[i].fixup_first=cursor; uint32_t cnt=0; while(cursor+cnt<g_nfix && g_fix[cursor+cnt].image_index==(uint32_t)i)cnt++; tbl[i].fixup_count=cnt; cursor+=cnt; } }

    memcpy(out+fixup_off, g_fix, fixup_bytes);
    if(g_extlen) memcpy(out+extern_off, g_extern, g_extlen);

    char tmp[700]; snprintf(tmp,sizeof tmp,"%s.tmp",outf);
    int fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd<0){perror("out");return 1;}
    if(write(fd,out,total)!=(ssize_t)total){perror("write");close(fd);unlink(tmp);return 1;}
    close(fd); if(rename(tmp,outf)){perror("rename");unlink(tmp);return 1;}

    fprintf(stderr,"OK DCC2: %d dylibs  RX=0x%llx RW=0x%llx RO=0x%llx total=0x%llx\n",np,
        (unsigned long long)g_rx,(unsigned long long)g_rw,(unsigned long long)g_ro,(unsigned long long)total);
    fprintf(stderr,"  fixups=%u  rebases=%d  binds=%d  [internal(ord)=%d self=%d flat-resolved=%d extern=%d]  extern-strtab=%uB\n",
        g_nfix,c_rebase,c_bind_internal+c_bind_self+c_bind_flat_resolved+c_bind_extern,
        c_bind_internal,c_bind_self,c_bind_flat_resolved,c_bind_extern,g_extlen);
    fprintf(stderr,"  UNRESOLVED (two-level/self that fell back to extern = suspicious)=%d\n",c_unresolved);
    return 0;
}
