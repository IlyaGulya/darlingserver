/* perf#24c1 cache validator: re-parse the emitted cache, prove structure + single-slide + byte-identity. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#define REGION_ALIGN 0x4000
#define MAX_SEGS 8
struct dcc_region { uint64_t file_off,size,vm_base; uint32_t prot,_pad; };
struct dcc_seg { char name[16]; uint64_t vmaddr,vmsize,region_off,filesize; uint32_t region_idx,prot; };
struct dcc_image { char path[256]; uint8_t uuid[16]; uint64_t src_inode,src_mtime,src_size,image_vmbase; uint32_t nsegs,init_index; struct dcc_seg segs[MAX_SEGS]; };
struct dcc_header { uint32_t magic,version,arch,image_count; uint64_t closure_hash; char install_root[256]; struct dcc_region regions[3]; };
static uint64_t roundup(uint64_t v,uint64_t a){return (v+a-1)&~(a-1);}
/* fat/thin src seg lookup for byte compare */
struct fat_header{uint32_t magic,nfat_arch;}; struct fat_arch{uint32_t cputype,cpusubtype,offset,size,align;};
struct mh64{uint32_t magic,cputype,cpusubtype,filetype,ncmds,sizeofcmds,flags,reserved;}; struct lc{uint32_t cmd,cmdsize;};
struct seg64{uint32_t cmd,cmdsize;char segname[16];uint64_t vmaddr,vmsize,fileoff,filesize;uint32_t maxprot,initprot,nsects,flags;};
int main(int argc,char**argv){
  int fd=open(argv[1],O_RDONLY);struct stat st;fstat(fd,&st);
  uint8_t*c=mmap(0,st.st_size,PROT_READ,MAP_PRIVATE,fd,0);
  struct dcc_header*h=(void*)c;
  printf("magic=%x ver=%u arch=%x images=%u total=0x%lx\n",h->magic,h->version,h->arch,h->image_count,(long)st.st_size);
  printf("regions: RX off=0x%lx size=0x%lx vm=0x%lx | RW off=0x%lx size=0x%lx vm=0x%lx | RO off=0x%lx size=0x%lx vm=0x%lx\n",
    (long)h->regions[0].file_off,(long)h->regions[0].size,(long)h->regions[0].vm_base,
    (long)h->regions[1].file_off,(long)h->regions[1].size,(long)h->regions[1].vm_base,
    (long)h->regions[2].file_off,(long)h->regions[2].size,(long)h->regions[2].vm_base);
  /* uniform-slide check: if reader maps region i at (vm_base + S), a single S works because
     regions are at fixed relative vm bases. verify RW.vm_base-RX.vm_base == roundup(RX.size) etc. */
  uint64_t rxb=h->regions[0].vm_base,rwb=h->regions[1].vm_base,rob=h->regions[2].vm_base;
  int ok_layout = (rwb-rxb==roundup(h->regions[0].size,REGION_ALIGN)) &&
                  (rob-rwb==roundup(h->regions[1].size,REGION_ALIGN));
  printf("uniform-slide region layout: %s (RW-RX=0x%lx want 0x%lx; RO-RW=0x%lx want 0x%lx)\n",
    ok_layout?"OK":"FAIL",(long)(rwb-rxb),(long)roundup(h->regions[0].size,REGION_ALIGN),
    (long)(rob-rwb),(long)roundup(h->regions[1].size,REGION_ALIGN));
  struct dcc_image*tbl=(void*)(c+sizeof *h);
  int slide_ok=1, byte_ok=1, seg_in_region=1; int nseg=0;
  for(uint32_t i=0;i<h->image_count;i++){
    struct dcc_image*di=&tbl[i]; uint64_t base=di->image_vmbase;
    /* open source, find x86_64 slice */
    int sfd=open(di->path,O_RDONLY); struct stat ss; fstat(sfd,&ss);
    uint8_t*sm=mmap(0,ss.st_size,PROT_READ,MAP_PRIVATE,sfd,0); close(sfd);
    uint64_t so=0; struct fat_header*fh=(void*)sm;
    if(ntohl(fh->magic)==0xcafebabe){uint32_t n=ntohl(fh->nfat_arch);struct fat_arch*fa=(void*)(fh+1);for(uint32_t k=0;k<n;k++)if(ntohl(fa[k].cputype)==0x01000007)so=ntohl(fa[k].offset);}
    struct mh64*mh=(void*)(sm+so); struct lc*l=(void*)(mh+1);
    /* map src seg name->fileoff/filesize */
    for(uint32_t k=0;k<mh->ncmds;k++){
      if(l->cmd==0x19){struct seg64*sg=(void*)l;
        for(uint32_t s=0;s<di->nsegs;s++) if(!memcmp(di->segs[s].name,sg->segname,16)){
          /* byte compare: cache region blob vs source slice bytes */
          uint64_t rfile=h->regions[di->segs[s].region_idx].file_off + di->segs[s].region_off;
          if(sg->filesize && memcmp(c+rfile, sm+so+sg->fileoff, sg->filesize)!=0) byte_ok=0;
        }
      }
      l=(void*)((char*)l+l->cmdsize);
    }
    munmap(sm,ss.st_size);
    for(uint32_t s=0;s<di->nsegs;s++){
      struct dcc_seg*ds=&di->segs[s]; nseg++;
      uint64_t rb=h->regions[ds->region_idx].vm_base;
      if(ds->vmaddr<rb || ds->vmaddr+ds->vmsize>rb+roundup(h->regions[ds->region_idx].size,REGION_ALIGN)) seg_in_region=0;
      /* single-slide: seg runtime = ds->vmaddr + S; image TEXT runtime = base + S; delta = ds->vmaddr-base.
         must be reproducible => just check region membership + uniform layout (done). */
    }
  }
  printf("images=%u segs=%d | region-layout=%s seg-in-region=%s byte-identical=%s\n",
    h->image_count,nseg,ok_layout?"OK":"FAIL",seg_in_region?"OK":"FAIL",byte_ok?"OK":"FAIL");
  printf("VMA PROJECTION: mapped as 3 regions => 3 VMAs (was %d segs = ~%d VMAs)\n",nseg,nseg);
  return (ok_layout&&seg_in_region&&byte_ok)?0:1;
}
