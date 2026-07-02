/* fxdump <cache> <image-substr>: list all fixups for an image, with the value that WILL be stored
 * (for REBASE: read stored qword; for BIND_INTERNAL: region[tgt]+off; for extern: sym). Flags any
 * resulting value that lands at an image_vmbase+0x68 (section-table "__text" zone) or in header (<__text). */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "/home/ilyagulya/work/darling-dev/darling/src/external/darlingserver/tools/closure-cache/dcc2-format.h"
static uint8_t*m; static struct dcc_header*h; static struct dcc_image*im;
static long vm2file(uint64_t va){ for(int r=0;r<3;r++){uint64_t b=h->regions[r].vm_base,e=b+h->regions[r].size; if(va>=b&&va<e)return(long)(h->regions[r].file_off+(va-b));} return -1; }
static const char* rname(int r){return r==0?"RX":r==1?"RW":r==2?"RO":"?";}
// find image whose __text (first section, at vmbase+? ) — we approximate header zone as [vmbase, vmbase+0x1000)
static void classify(uint64_t va,char*out){
  const char*rn[3]={"RX","RW","RO"}; int reg=-1;
  for(int r=0;r<3;r++) if(va>=h->regions[r].vm_base&&va<h->regions[r].vm_base+h->regions[r].size)reg=r;
  if(reg<0){sprintf(out,"OUTSIDE");return;}
  for(uint32_t j=0;j<h->image_count;j++) for(uint32_t sg=0;sg<im[j].nsegs;sg++){
    uint64_t b=im[j].segs[sg].vmaddr,e=b+im[j].segs[sg].vmsize;
    if(va>=b&&va<e){ uint64_t off=va-b; const char*flag="";
      if(sg==0 && off<0x1000) flag=" <<<HEADER/SECTTABLE ZONE";
      sprintf(out,"%s %s %.16s +0x%llx%s",rn[reg],strrchr(im[j].path,'/')+1,im[j].segs[sg].name,(unsigned long long)off,flag); return; }
  }
  sprintf(out,"%s(no-seg)",rn[reg]);
}
int main(int c,char**v){
 int fd=open(v[1],O_RDONLY);struct stat s;fstat(fd,&s);
 m=mmap(0,s.st_size,PROT_READ,MAP_PRIVATE,fd,0); h=(void*)m; im=(void*)(m+sizeof *h);
 struct dcc_fixup*fx=(void*)(m+h->fixup_off);
 int tgt=-1; for(uint32_t i=0;i<h->image_count;i++) if(strstr(im[i].path,v[2])){tgt=i;break;}
 if(tgt<0){printf("no image\n");return 1;}
 printf("IMAGE[%d] %s vmbase=0x%llx\n",tgt,im[tgt].path,(unsigned long long)im[tgt].image_vmbase);
 int nreb=0,nbi=0,nbe=0,nbel=0,flagged=0;
 char cl[160];
 for(uint32_t i=0;i<h->fixup_count;i++){ if(fx[i].image_index!=(uint32_t)tgt)continue;
   uint64_t locva=0; // loc region base + loc_off
   locva=h->regions[fx[i].loc_region].vm_base + fx[i].loc_off;
   if(fx[i].kind==DCC_FIX_REBASE){ nreb++;
     long fo=vm2file(locva); uint64_t stored=fo>=0?*(uint64_t*)(m+fo):0; classify(stored,cl);
     if(strstr(cl,"HEADER")){ printf("  REBASE loc=%s+0x%llx stored=0x%llx -> %s\n",rname(fx[i].loc_region),(unsigned long long)fx[i].loc_off,(unsigned long long)stored,cl); flagged++; }
   } else if(fx[i].kind==DCC_FIX_BIND_INTERNAL){ nbi++;
     uint64_t tv=h->regions[fx[i].tgt_region].vm_base + fx[i].tgt_off; classify(tv,cl);
     if(strstr(cl,"HEADER")){ printf("  BIND_INT loc=%s+0x%llx -> tgt=0x%llx %s\n",rname(fx[i].loc_region),(unsigned long long)fx[i].loc_off,(unsigned long long)tv,cl); flagged++; }
   } else if(fx[i].kind==DCC_FIX_BIND_EXTERN){ nbe++; }
   else if(fx[i].kind==DCC_FIX_BIND_EXTERN_LAZY){ nbel++; }
 }
 printf("TOTAL for image: rebase=%d bind_int=%d bind_ext=%d bind_ext_lazy=%d  HEADER-ZONE-FLAGGED=%d\n",nreb,nbi,nbe,nbel,flagged);
 return 0;
}
