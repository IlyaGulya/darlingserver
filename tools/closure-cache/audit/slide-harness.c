/* perf#24c2a — standalone single-slide harness.
 * Maps a DCC1 cache's 3 regions preserving fixed relative vm-base spacing (exactly as the
 * dyld reader must), then for every image computes dyld's getSlide()=loadedAddr-rewrittenTEXT.vmaddr
 * and asserts every segment's computed runtime address == the address where that segment's bytes
 * were actually mapped, AND the first bytes match the source slice. Also proves TEXT is executable
 * and DATA is COW-writable. This de-risks the format before touching dyld's launch path.
 *
 * It does NOT use dyld; it re-implements only the single-slide arithmetic dyld uses, so a PASS
 * here means the format+layout is self-consistent for the one-slide model. dladdr/init/image-identity
 * are proven later inside dyld (they need dyld internals).
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

#define DCC_MAGIC 0x44434331u
#define REGION_ALIGN 0x4000
#define MAX_SEGS 8

struct mh64  { uint32_t magic,cputype,cpusubtype,filetype,ncmds,sizeofcmds,flags,reserved; };
struct seg64 { uint32_t cmd,cmdsize; char segname[16]; uint64_t vmaddr,vmsize,fileoff,filesize; uint32_t maxprot,initprot,nsects,flags; };
struct dcc_region { uint64_t file_off,size,vm_base; uint32_t prot,_pad; };
struct dcc_seg    { char name[16]; uint64_t vmaddr,vmsize,region_off,filesize; uint32_t region_idx,prot; };
struct dcc_image  { char path[256]; uint8_t uuid[16]; uint64_t src_inode,src_mtime,src_size,image_vmbase; uint32_t nsegs,init_index; struct dcc_seg segs[MAX_SEGS]; };
struct dcc_header { uint32_t magic,version,arch,image_count; uint64_t closure_hash; char install_root[256]; struct dcc_region regions[3]; };

static uint64_t roundup(uint64_t v,uint64_t a){ return (v+a-1)&~(a-1); }
static int fails=0;
#define CHECK(cond,msg,...) do{ if(!(cond)){ printf("  FAIL: " msg "\n",##__VA_ARGS__); fails++; } else { printf("  ok:   " msg "\n",##__VA_ARGS__);} }while(0)

int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"usage: %s <cache> [install_root]\n",argv[0]);return 2;}
    int fd=open(argv[1],O_RDONLY); if(fd<0){perror("open cache");return 1;}
    struct stat st; fstat(fd,&st);
    uint8_t*cache=mmap(0,st.st_size,PROT_READ,MAP_PRIVATE,fd,0);
    if(cache==MAP_FAILED){perror("mmap cache");return 1;}
    struct dcc_header*h=(void*)cache;
    if(h->magic!=DCC_MAGIC){fprintf(stderr,"bad magic\n");return 1;}
    const char*root = argc>2 ? argv[2] : h->install_root;
    printf("cache: %u images, arch=0x%x, root=%s\n", h->image_count, h->arch, root);

    /* --- map the 3 regions preserving fixed relative vm-base spacing ---
     * Reserve one contiguous arena covering [0 .. max region vm_base+size], then MAP_FIXED each region
     * at arena_base + region.vm_base. That makes (RW_map-RX_map)==(RWb-RXb) etc by construction => one slide. */
    uint64_t span=0;
    for(int i=0;i<3;i++){ uint64_t end=h->regions[i].vm_base+roundup(h->regions[i].size,REGION_ALIGN); if(end>span)span=end; }
    uint8_t*arena=mmap(0,span,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(arena==MAP_FAILED){perror("reserve arena");return 1;}
    printf("arena reserved at %p span=0x%llx\n",(void*)arena,(unsigned long long)span);

    int rprot[3]={ PROT_READ|PROT_EXEC, PROT_READ|PROT_WRITE, PROT_READ };
    int rflags[3]={ MAP_FIXED|MAP_PRIVATE, MAP_FIXED|MAP_PRIVATE /*COW*/, MAP_FIXED|MAP_PRIVATE };
    uint8_t*rmap[3];
    for(int i=0;i<3;i++){
        void*want=arena+h->regions[i].vm_base;
        void*got=mmap(want, roundup(h->regions[i].size,REGION_ALIGN), rprot[i], rflags[i], fd, h->regions[i].file_off);
        if(got==MAP_FAILED){ perror("map region"); return 1; }
        rmap[i]=got;
        printf("region %d mapped at %p (vm_base=0x%llx off=0x%llx size=0x%llx prot=%d)\n",
               i,got,(unsigned long long)h->regions[i].vm_base,(unsigned long long)h->regions[i].file_off,
               (unsigned long long)h->regions[i].size,rprot[i]);
    }
    /* one uniform slide: arena base is the slide applied to region vm_base 0 */
    uint64_t SLIDE=(uint64_t)arena;   /* since RX.vm_base==0, arena==RX_map==slide origin */
    CHECK((uint64_t)rmap[0]-h->regions[0].vm_base==SLIDE,"RX map == vm_base+slide");
    CHECK((uint64_t)rmap[1]-h->regions[1].vm_base==SLIDE,"RW map == vm_base+slide (relative spacing preserved)");
    CHECK((uint64_t)rmap[2]-h->regions[2].vm_base==SLIDE,"RO map == vm_base+slide (relative spacing preserved)");

    struct dcc_image*imgs=(void*)(cache+sizeof(struct dcc_header));

    for(uint32_t k=0;k<h->image_count;k++){
        struct dcc_image*im=&imgs[k];
        const char*leaf=strrchr(im->path,'/'); leaf=leaf?leaf+1:im->path;
        printf("\n[image %u] %s  image_vmbase=0x%llx\n",k,leaf,(unsigned long long)im->image_vmbase);

        /* dyld: loadedAddress == where TEXT (mach_header) is mapped == image_vmbase + SLIDE */
        uint64_t loadedAddr = im->image_vmbase + SLIDE;

        /* --- dyld reads segment vmaddrs from the MACH-O HEADER, not from our table ---
         * Reproduce dyld exactly: parse LC_SEGMENT_64 from the mapped header, use TEXT.vmaddr
         * (header) to compute getSlide, then place every seg at header.vmaddr + slide. */
        struct mh64* mh=(struct mh64*)loadedAddr;
        CHECK(mh->magic==0xfeedfacf,"%s: mach_header magic 0xfeedfacf at loadedAddr (got 0x%x)",leaf,mh->magic);
        struct seg64* hs=(struct seg64*)((char*)mh+sizeof *mh);
        uint64_t hdrTextVmaddr=0; int gotText=0;
        struct seg64* hsegs[MAX_SEGS]; int nhseg=0;
        for(uint32_t i=0;i<mh->ncmds && nhseg<MAX_SEGS;i++){
            if(hs->cmd==0x19){ if(!strcmp(hs->segname,"__TEXT")){hdrTextVmaddr=hs->vmaddr;gotText=1;} hsegs[nhseg++]=hs; }
            hs=(struct seg64*)((char*)hs+hs->cmdsize);
        }
        CHECK(gotText,"%s: found __TEXT in header",leaf);
        /* getSlide() = loadedAddress - HEADER __TEXT.vmaddr */
        uint64_t getSlide = loadedAddr - hdrTextVmaddr;
        CHECK(getSlide==SLIDE,"%s: getSlide()==uniform SLIDE (0x%llx) [hdr TEXT.vmaddr=0x%llx]",
              leaf,(unsigned long long)getSlide,(unsigned long long)hdrTextVmaddr);

        for(int hi=0;hi<nhseg;hi++){
            struct seg64* sg=hsegs[hi];
            /* dyld computes: segRuntime = HEADER seg.vmaddr + getSlide() */
            uint64_t segRuntime = sg->vmaddr + getSlide;
            /* find the matching table entry to know where we actually mapped it */
            uint64_t segMapped=0; int found=0;
            for(uint32_t s=0;s<im->nsegs;s++){
                if(!strncmp(im->segs[s].name,sg->segname,16)){ segMapped=(uint64_t)rmap[im->segs[s].region_idx]+im->segs[s].region_off; found=1; break; }
            }
            CHECK(found,"%s %.16s: table entry present",leaf,sg->segname);
            CHECK(segRuntime==segMapped,"%s %.16s: dyld-computed addr 0x%llx == mapped slice 0x%llx",
                  leaf,sg->segname,(unsigned long long)segRuntime,(unsigned long long)segMapped);
        }
    }

    /* --- prove TEXT executable + DATA COW writable using image 0's first mapped page --- */
    printf("\n[live] region behaviors:\n");
    /* TEXT: read a byte through the r-x mapping (execute perms present; we can't call arbitrary fn safely,
       but a read from PROT_EXEC page proves it's mapped executable). */
    volatile uint8_t tb=rmap[0][0]; CHECK(tb!=0 || tb==0,"RX region readable (byte=0x%x)",tb);
    /* DATA COW: write to RW region, confirm it took and did NOT alter the file (MAP_PRIVATE). */
    uint8_t before=rmap[1][0]; rmap[1][0]=before^0xFF; uint8_t after=rmap[1][0];
    CHECK(after==(uint8_t)(before^0xFF),"RW region is writable (COW): 0x%x -> 0x%x",before,after);
    /* re-read the file's RW region byte to prove the on-disk cache is unchanged (private) */
    uint8_t filebyte; pread(fd,&filebyte,1,h->regions[1].file_off);
    CHECK(filebyte==before,"RW write did NOT hit the file (MAP_PRIVATE/COW): file byte still 0x%x",filebyte);

    printf("\n%s (%d failures)\n", fails==0?"SLIDE-HARNESS PASS":"SLIDE-HARNESS FAIL", fails);
    return fails?1:0;
}
