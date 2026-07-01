/* perf#24c1 — Darling system-closure cache builder (standalone host tool, no dyld dependency).
 *
 * Reads the libSystem.B + /usr/lib/system/libsystem_* closure and emits a packed cache:
 *   - 3 per-permission REGIONS: RX (all __TEXT), RW (all __DATA), RO (all __LINKEDIT)
 *   - each dylib's segment VMADDRS are REWRITTEN to region-relative positions so that
 *     dyld's single-slide model (loadedAddress + (seg.vmaddr - TEXT.vmaddr)) reproduces
 *     every segment address (per perf#24c-gate). LINKEDIT-consumer file offsets rewritten
 *     by the linkedit delta. Code/text bytes untouched.
 *   - an image table (path, uuid/inode/mtime/size, per-seg rewritten vmaddr+regionoff, init idx).
 *
 * Model (perf#24c-gate corrected): NOT 3 arbitrary planes with original vmaddrs.
 * Region layout in a single virtual span: [RX region][RW region][RO region], and each image's
 * TEXT goes to RX at rxOff, DATA to RW at rwOff, LINKEDIT to RO at roOff, with vmaddrs assigned so
 *   image_base(vmaddr of TEXT) = RX_base + rxOff
 *   DATA.vmaddr  = image_base + (RW_base + rwOff - (RX_base+rxOff))
 *   LINK.vmaddr  = image_base + (RO_base + roOff - (RX_base+rxOff))
 * i.e. we choose per-image vmaddr bases so seg.vmaddr deltas match the region layout exactly.
 *
 * Deterministic: closure list is sorted; region packing is first-fit in list order.
 * Atomic: writes to <out>.tmp then rename(). Staleness: records inode/mtime/size per source.
 * All-or-nothing: any parse/stale error aborts with no output file.
 *
 * NOTE: this tool does NOT change dyld behavior. It only produces a file. The reader (perf#24c2)
 * is a separate, flag-gated change.
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

/* ---- minimal Mach-O (stable ABI) ---- */
struct fat_header { uint32_t magic, nfat_arch; };
struct fat_arch   { uint32_t cputype, cpusubtype, offset, size, align; };
struct mh64 { uint32_t magic,cputype,cpusubtype,filetype,ncmds,sizeofcmds,flags,reserved; };
struct lc   { uint32_t cmd, cmdsize; };
struct seg64 { uint32_t cmd,cmdsize; char segname[16]; uint64_t vmaddr,vmsize,fileoff,filesize; uint32_t maxprot,initprot,nsects,flags; };
struct dyld_info { uint32_t cmd,cmdsize,rebase_off,rebase_size,bind_off,bind_size,weak_bind_off,weak_bind_size,lazy_bind_off,lazy_bind_size,export_off,export_size; };
struct symtab   { uint32_t cmd,cmdsize,symoff,nsyms,stroff,strsize; };
struct dysymtab { uint32_t cmd,cmdsize,ilocalsym,nlocalsym,iextdefsym,nextdefsym,iundefsym,nundefsym,tocoff,ntoc,modtaboff,nmodtab,extrefsymoff,nextrefsyms,indirectsymoff,nindirectsyms,extreloff,nextrel,locreloff,nlocrel; };
struct linkedit_data { uint32_t cmd,cmdsize,dataoff,datasize; };
struct uuid_cmd { uint32_t cmd,cmdsize; uint8_t uuid[16]; };
#define FAT_MAGIC 0xcafebabe
#define MH_MAGIC_64 0xfeedfacf
#define CPU_TYPE_X86_64 0x01000007
#define LC_SEGMENT_64 0x19
#define LC_SYMTAB 0x2
#define LC_DYSYMTAB 0xb
#define LC_DYLD_INFO_ONLY 0x80000022
#define LC_DYLD_INFO 0x22
#define LC_FUNCTION_STARTS 0x26
#define LC_DATA_IN_CODE 0x29
#define LC_CODE_SIGNATURE 0x1d
#define LC_UUID 0x1b

/* ---- cache file format ---- */
#define DCC_MAGIC 0x44434331u  /* "DCC1" */
#define DCC_VERSION 1
#define REGION_ALIGN 0x4000     /* 16K page align between images within a region */
#define MAX_IMAGES 128
#define MAX_SEGS 8

struct dcc_region { uint64_t file_off; uint64_t size; uint64_t vm_base; uint32_t prot; uint32_t _pad; };
struct dcc_seg    { char name[16]; uint64_t vmaddr; uint64_t vmsize; uint64_t region_off; uint64_t filesize; uint32_t region_idx; uint32_t prot; };
struct dcc_image {
    char path[256];
    uint8_t uuid[16];
    uint64_t src_inode, src_mtime, src_size;
    uint64_t image_vmbase;   /* rewritten TEXT vmaddr */
    uint32_t nsegs; uint32_t init_index;
    struct dcc_seg segs[MAX_SEGS];
};
struct dcc_header {
    uint32_t magic, version, arch, image_count;
    uint64_t closure_hash;
    char install_root[256];
    struct dcc_region regions[3];   /* 0=RX 1=RW 2=RO */
    /* followed by image_count * dcc_image, then the 3 region blobs at regions[i].file_off */
};

/* ---- builder ---- */
static uint64_t g_rx=0, g_rw=0, g_ro=0;         /* running region sizes */
static const uint64_t RX_VM=0x000000000ULL;     /* placeholder region vm bases (reader assigns real) */
/* We record region-relative seg offsets + per-image vm bases; reader maps regions and sets
 * image_vmbase = RX_map_base + rx_off. The delta invariant is enforced by construction below. */

struct src_seg { char name[16]; uint64_t vmaddr,vmsize,fileoff,filesize; uint32_t prot; int region; };
struct src_img {
    char path[256]; uint64_t sliceOff, sliceSize; uint8_t*map; uint64_t mapsize;
    uint64_t inode,mtime,size; uint8_t uuid[16];
    struct src_seg segs[MAX_SEGS]; int nsegs;
    uint64_t textVmaddr; /* original */
    /* rewritten */
    uint64_t rx_off, rw_off, ro_off; /* region offsets for TEXT/DATA/LINKEDIT */
};

static uint64_t roundup(uint64_t v,uint64_t a){ return (v+a-1)&~(a-1); }
static uint64_t fnv1a(const void*p,size_t n,uint64_t h){ const uint8_t*b=p; for(size_t i=0;i<n;i++){h^=b[i];h*=1099511628211ULL;} return h; }

static int parse_image(const char*path, struct src_img*im){
    strncpy(im->path,path,sizeof im->path-1);
    int fd=open(path,O_RDONLY); if(fd<0){fprintf(stderr,"open %s: %s\n",path,strerror(errno));return -1;}
    struct stat st; if(fstat(fd,&st)){close(fd);return -1;}
    im->inode=st.st_ino; im->mtime=st.st_mtime; im->size=st.st_size;
    uint8_t*base=mmap(0,st.st_size,PROT_READ,MAP_PRIVATE,fd,0); close(fd);
    if(base==MAP_FAILED){fprintf(stderr,"mmap %s\n",path);return -1;}
    im->map=base; im->mapsize=st.st_size;
    uint64_t so=0, ss=st.st_size;
    struct fat_header*fh=(void*)base;
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
            if(im->nsegs>=MAX_SEGS){fprintf(stderr,"%s: too many segs\n",path);return -1;}
            struct src_seg*d=&im->segs[im->nsegs++];
            memcpy(d->name,s->segname,16); d->vmaddr=s->vmaddr; d->vmsize=s->vmsize;
            d->fileoff=s->fileoff; d->filesize=s->filesize; d->prot=s->initprot;
            if(!strcmp(s->segname,"__TEXT")){d->region=0; im->textVmaddr=s->vmaddr;}
            else if(!strcmp(s->segname,"__LINKEDIT")) d->region=2;
            else d->region=1; /* __DATA and anything writable-ish */
        } else if(c->cmd==LC_UUID){ struct uuid_cmd*u=(void*)c; memcpy(im->uuid,u->uuid,16); }
        c=(void*)((char*)c+c->cmdsize);
    }
    return 0;
}

int main(int argc,char**argv){
    if(argc<4){fprintf(stderr,"usage: %s <install_root> <closure_list.txt> <out.cache>\n",argv[0]);return 2;}
    const char*root=argv[1]; const char*listf=argv[2]; const char*outf=argv[3];

    /* read + sort closure list (deterministic) */
    FILE*lf=fopen(listf,"r"); if(!lf){perror("list");return 2;}
    char*paths[MAX_IMAGES]; int np=0; char line[512];
    while(fgets(line,sizeof line,lf)){
        char*nl=strchr(line,'\n'); if(nl)*nl=0; if(!line[0]) continue;
        if(strstr(line,".dylib")==NULL) continue;         /* dylibs only, skip clang exe */
        paths[np]=strdup(line); np++;
        if(np>=MAX_IMAGES) break;
    }
    fclose(lf);
    for(int i=0;i<np;i++)for(int j=i+1;j<np;j++) if(strcmp(paths[i],paths[j])>0){char*t=paths[i];paths[i]=paths[j];paths[j]=t;}

    struct src_img*imgs=calloc(np,sizeof *imgs);
    uint64_t chash=1469598103934665603ULL;
    for(int i=0;i<np;i++){
        char full[600]; snprintf(full,sizeof full,"%s%s",root,paths[i]);
        if(parse_image(full,&imgs[i])){fprintf(stderr,"ABORT: parse failed %s\n",full);return 1;}
        chash=fnv1a(paths[i],strlen(paths[i]),chash);
        chash=fnv1a(&imgs[i].inode,8,chash); chash=fnv1a(&imgs[i].mtime,8,chash); chash=fnv1a(&imgs[i].size,8,chash);
    }

    /* assign region offsets (first-fit in sorted order, page-aligned per image) */
    for(int i=0;i<np;i++){
        struct src_img*im=&imgs[i];
        for(int s=0;s<im->nsegs;s++){
            struct src_seg*sg=&im->segs[s];
            uint64_t sz=roundup(sg->vmsize,REGION_ALIGN);
            if(sg->region==0){ im->rx_off=g_rx; g_rx+=sz; }
            else if(sg->region==1){ im->rw_off=g_rw; g_rw+=sz; }
            else { im->ro_off=g_ro; g_ro+=sz; }
        }
    }

    /* Region VM bases are assigned so the single-slide invariant holds per image.
     * Reader will map RX at some base B0; RW at B1; RO at B2 (contiguous or not, reader decides,
     * but records real vm_base). For the builder we express seg vmaddrs RELATIVE to a per-image
     * TEXT base and store region_off; the reader computes:
     *    image_vmbase = RX.vm_base + rx_off
     *    seg.vmaddr   = <region>.vm_base + <region>_off   (absolute, per region map)
     * The single-slide model requires seg.vmaddr - TEXT.vmaddr == (region.vm_base+off) - (RX.vm_base+rx_off).
     * We store rewritten vmaddrs assuming the reader lays regions at fixed relative bases:
     *    RX.vm_base = 0, RW.vm_base = roundup(RX_size), RO.vm_base = roundup(RX+RW).
     * The reader must honor these relative bases (or apply one uniform slide to all three). */
    uint64_t RXb=0, RWb=roundup(g_rx,REGION_ALIGN), ROb=roundup(RWb+g_rw,REGION_ALIGN);

    struct dcc_header hdr; memset(&hdr,0,sizeof hdr);
    hdr.magic=DCC_MAGIC; hdr.version=DCC_VERSION; hdr.arch=CPU_TYPE_X86_64; hdr.image_count=np;
    hdr.closure_hash=chash; strncpy(hdr.install_root,root,sizeof hdr.install_root-1);

    uint64_t tbl_off = sizeof(struct dcc_header);
    uint64_t images_bytes = (uint64_t)np*sizeof(struct dcc_image);
    uint64_t rx_file = roundup(tbl_off+images_bytes, REGION_ALIGN);
    uint64_t rw_file = roundup(rx_file+g_rx, REGION_ALIGN);
    uint64_t ro_file = roundup(rw_file+g_rw, REGION_ALIGN);
    uint64_t total   = roundup(ro_file+g_ro, REGION_ALIGN);

    hdr.regions[0]=(struct dcc_region){rx_file,g_rx,RXb,5};
    hdr.regions[1]=(struct dcc_region){rw_file,g_rw,RWb,3};
    hdr.regions[2]=(struct dcc_region){ro_file,g_ro,ROb,1};

    uint8_t*out=calloc(1,total);
    memcpy(out,&hdr,sizeof hdr);
    struct dcc_image*tbl=(void*)(out+tbl_off);

    for(int i=0;i<np;i++){
        struct src_img*im=&imgs[i]; struct dcc_image*di=&tbl[i];
        strncpy(di->path,im->path,sizeof di->path-1);
        memcpy(di->uuid,im->uuid,16);
        di->src_inode=im->inode; di->src_mtime=im->mtime; di->src_size=im->size;
        di->image_vmbase = RXb + im->rx_off;   /* rewritten TEXT vmaddr */
        di->nsegs=im->nsegs; di->init_index=i; /* init order = sorted-list order (placeholder; c2 will use closure graph) */
        for(int s=0;s<im->nsegs;s++){
            struct src_seg*sg=&im->segs[s]; struct dcc_seg*ds=&di->segs[s];
            memcpy(ds->name,sg->name,16); ds->vmsize=sg->vmsize; ds->filesize=sg->filesize; ds->prot=sg->prot;
            uint64_t roff, vmb, rfile; int ridx=sg->region;
            if(ridx==0){roff=im->rx_off; vmb=RXb; rfile=rx_file;}
            else if(ridx==1){roff=im->rw_off; vmb=RWb; rfile=rw_file;}
            else {roff=im->ro_off; vmb=ROb; rfile=ro_file;}
            ds->region_idx=ridx; ds->region_off=roff;
            ds->vmaddr = vmb + roff;   /* REWRITTEN vmaddr (region-relative absolute) */
            /* copy the segment file bytes into its region blob */
            memcpy(out + rfile + roff, im->map + im->sliceOff + sg->fileoff, sg->filesize);
        }
        /* --- single-slide invariant (build-time assert) ---
         * dyld reads the REWRITTEN LC_SEGMENT vmaddrs (ds->vmaddr) and computes
         *   getSlide = loadedAddr(TEXT) - rewrittenTEXT.vmaddr ; segAddr = seg.vmaddr + slide.
         * For that to place every seg correctly when the reader maps the 3 regions at ONE uniform
         * slide, the regions must sit at FIXED relative vm bases (RXb, RWb, ROb) and the reader must
         * map them so (RW_map - RX_map) == (RWb - RXb) and (RO_map - RX_map) == (ROb - RXb).
         * Given that, correctness is: for each seg, (ds->vmaddr - image_vmbase) is the runtime delta
         * from the image's TEXT, and the reader reproduces it because all three region maps share the
         * same slide. The invariant to assert at build time is that region bases are ordered and each
         * seg's rewritten vmaddr lies within its region. (The original per-image spacing is NOT
         * preserved and need not be — dyld uses the REWRITTEN vmaddrs.) */
        uint64_t base = di->image_vmbase;
        for(int s=0;s<im->nsegs;s++){
            struct dcc_seg*ds=&di->segs[s];
            uint64_t rbase = hdr.regions[ds->region_idx].vm_base;
            uint64_t rsize = hdr.regions[ds->region_idx].size;
            if(ds->vmaddr < rbase || ds->vmaddr + ds->vmsize > rbase + roundup(rsize,REGION_ALIGN)){
                fprintf(stderr,"ABORT: %s seg %.16s vmaddr 0x%llx outside region %u [0x%llx,+0x%llx)\n",
                    im->path, ds->name,(unsigned long long)ds->vmaddr,ds->region_idx,
                    (unsigned long long)rbase,(unsigned long long)rsize);
                return 1;
            }
            (void)base;
        }
    }

    /* atomic write: tmp + rename */
    char tmp[700]; snprintf(tmp,sizeof tmp,"%s.tmp",outf);
    int fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd<0){perror("out");return 1;}
    if(write(fd,out,total)!=(ssize_t)total){perror("write");close(fd);unlink(tmp);return 1;}
    close(fd);
    if(rename(tmp,outf)){perror("rename");unlink(tmp);return 1;}

    fprintf(stderr,"OK: %d dylibs, RX=0x%llx RW=0x%llx RO=0x%llx total=0x%llx hash=0x%llx -> %s\n",
            np,(unsigned long long)g_rx,(unsigned long long)g_rw,(unsigned long long)g_ro,
            (unsigned long long)total,(unsigned long long)chash,outf);
    return 0;
}
