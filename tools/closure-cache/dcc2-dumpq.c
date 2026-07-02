/* dumpq <cache> <image-substr> <seg-rel-vmaddr-start> <count>
 * Dumps <count> qwords starting at the given VMADDR (cache-relative absolute, as rewritten).
 * Classifies each value against regions and reports owning image+section if it lands in RX. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "/home/ilyagulya/work/darling-dev/darling/src/external/darlingserver/tools/closure-cache/dcc2-format.h"
struct mh64{uint32_t magic,cputype,cpusubtype,filetype,ncmds,sizeofcmds,flags,reserved;};
struct lc{uint32_t cmd,cmdsize;};
struct seg64{uint32_t cmd,cmdsize;char segname[16];uint64_t vmaddr,vmsize,fileoff,filesize;uint32_t maxprot,initprot,nsects,flags;};
struct sect64{char sectname[16],segname[16];uint64_t addr,size;uint32_t offset,align,reloff,nreloc,flags,r1,r2,r3;};
static uint8_t*m; static struct dcc_header*h; static struct dcc_image*im;
// map a cache-relative-absolute vmaddr to (file offset). Uses region vm_base ranges.
static long vm2file(uint64_t va){
  for(int r=0;r<3;r++){ uint64_t b=h->regions[r].vm_base,e=b+h->regions[r].size;
    if(va>=b&&va<e) return (long)(h->regions[r].file_off + (va-b)); }
  return -1;
}
static void classify(uint64_t va, char*out){
  const char*rn[3]={"RX","RW","RO"};
  int reg=-1; for(int r=0;r<3;r++) if(va>=h->regions[r].vm_base && va<h->regions[r].vm_base+h->regions[r].size)reg=r;
  if(reg<0){ sprintf(out,"OUTSIDE-CACHE"); return; }
  // owning image/segment
  for(uint32_t j=0;j<h->image_count;j++) for(uint32_t sg=0;sg<im[j].nsegs;sg++){
    uint64_t b=im[j].segs[sg].vmaddr,e=b+im[j].segs[sg].vmsize;
    if(va>=b&&va<e){ sprintf(out,"%s %s %.16s +0x%llx",rn[reg],strrchr(im[j].path,'/')+1,im[j].segs[sg].name,(unsigned long long)(va-b)); return; }
  }
  sprintf(out,"%s (region only, no seg)",rn[reg]);
}
int main(int c,char**v){
 int fd=open(v[1],O_RDONLY);struct stat s;fstat(fd,&s);
 m=mmap(0,s.st_size,PROT_READ,MAP_PRIVATE,fd,0);
 h=(void*)m; im=(void*)(m+sizeof *h);
 uint64_t va=strtoull(v[3],0,0); int n=atoi(v[4]);
 char cl[128];
 for(int i=0;i<n;i++){ uint64_t a=va+i*8; long fo=vm2file(a);
   if(fo<0){ printf("  [0x%llx] <not mapped>\n",(unsigned long long)a); continue; }
   uint64_t val=*(uint64_t*)(m+fo);
   classify(val,cl);
   printf("  [0x%llx] = 0x%016llx   %s\n",(unsigned long long)a,(unsigned long long)val,cl);
 }
 return 0;
}
