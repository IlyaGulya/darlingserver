#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string.h>
struct dcc_region{uint64_t file_off,size,vm_base;uint32_t prot,_pad;};
struct dcc_seg{char name[16];uint64_t vmaddr,vmsize,region_off,filesize;uint32_t region_idx,prot;};
struct dcc_image{char path[256];uint8_t uuid[16];uint64_t src_inode,src_mtime,src_size,image_vmbase;uint32_t nsegs,init_index;struct dcc_seg segs[8];};
struct dcc_header{uint32_t magic,version,arch,image_count;uint64_t closure_hash;char install_root[256];struct dcc_region regions[3];};
struct mh64{uint32_t magic,cputype,cpusubtype,filetype,ncmds,sizeofcmds,flags,reserved;};
struct seg64{uint32_t cmd,cmdsize;char segname[16];uint64_t vmaddr,vmsize,fileoff,filesize;uint32_t maxprot,initprot,nsects,flags;};
int main(int c,char**v){
  int fd=open(v[1],O_RDONLY);struct stat st;fstat(fd,&st);
  uint8_t*m=mmap(0,st.st_size,PROT_READ,MAP_PRIVATE,fd,0);
  struct dcc_header*h=(void*)m; struct dcc_image*im=(void*)(m+sizeof *h);
  for(uint32_t k=0;k<h->image_count;k++){
    struct dcc_image*d=&im[k];
    // mach header of image k is in RX region at region_off of its TEXT seg
    uint64_t rxfile=h->regions[0].file_off;
    struct mh64*mh=(void*)(m+rxfile+d->segs[0].region_off); // seg 0 is TEXT
    printf("[%s] table image_vmbase=0x%llx\n", strrchr(d->path,'/')+1,(unsigned long long)d->image_vmbase);
    struct seg64* s=(void*)((char*)mh+sizeof *mh);
    for(uint32_t i=0;i<mh->ncmds;i++){
      if(s->cmd==0x19){ printf("   HEADER LC_SEGMENT_64 %-16.16s vmaddr=0x%llx\n",s->segname,(unsigned long long)s->vmaddr); }
      s=(void*)((char*)s+s->cmdsize);
    }
    for(uint32_t i=0;i<d->nsegs;i++) printf("   TABLE  %-16.16s vmaddr=0x%llx\n",d->segs[i].name,(unsigned long long)d->segs[i].vmaddr);
  }
  return 0;
}
