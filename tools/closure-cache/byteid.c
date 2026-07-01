#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string.h>
#include <arpa/inet.h>
#include "dcc2-format.h"
struct fat_header{uint32_t magic,nfat_arch;};
struct fat_arch{uint32_t cputype,cpusubtype,offset,size,align;};
struct mh64{uint32_t magic,cputype,cpusubtype,filetype,ncmds,sizeofcmds,flags,reserved;};
struct seg64{uint32_t cmd,cmdsize;char segname[16];uint64_t vmaddr,vmsize,fileoff,filesize;uint32_t maxprot,initprot,nsects,flags;};
int main(int c,char**v){
  int fd=open(v[1],O_RDONLY);struct stat st;fstat(fd,&st);
  uint8_t*cache=mmap(0,st.st_size,PROT_READ,MAP_PRIVATE,fd,0);close(fd);
  struct dcc_header*h=(void*)cache;struct dcc_image*im=(void*)(cache+sizeof *h);
  int datam=0,linkm=0,textbodym=0,checked=0;
  for(uint32_t k=0;k<h->image_count;k++){
    struct dcc_image*d=&im[k];
    char full[600];strncpy(full,d->path,sizeof full-1);
    int f2=open(full,O_RDONLY);if(f2<0){perror(full);continue;}
    struct stat s2;fstat(f2,&s2);uint8_t*src=mmap(0,s2.st_size,PROT_READ,MAP_PRIVATE,f2,0);close(f2);
    uint64_t so=0;struct fat_header*fh=(void*)src;
    if(ntohl(fh->magic)==0xcafebabe){uint32_t n=ntohl(fh->nfat_arch);struct fat_arch*fa=(void*)(fh+1);for(uint32_t i=0;i<n;i++)if(ntohl(fa[i].cputype)==0x01000007)so=ntohl(fa[i].offset);}
    struct mh64*sh=(void*)(src+so);
    for(uint32_t sg=0;sg<d->nsegs;sg++){
      struct dcc_seg*ds=&d->segs[sg];
      uint64_t srcfoff=0,srcfsz=0;
      char*lc=(char*)sh+sizeof *sh;
      for(uint32_t i=0;i<sh->ncmds;i++){struct seg64*s=(void*)lc;if(s->cmd==0x19&&!strncmp(s->segname,ds->name,16)){srcfoff=s->fileoff;srcfsz=s->filesize;}lc+=((struct dcc_seg*)0,*(uint32_t*)(lc+4));}
      uint8_t*packed=cache+h->regions[ds->region_idx].file_off+ds->region_off;
      uint8_t*srcseg=src+so+srcfoff;
      uint64_t n=srcfsz<ds->filesize?srcfsz:ds->filesize;
      if(!strcmp(ds->name,"__DATA")){if(memcmp(packed,srcseg,n))datam++;}
      else if(!strcmp(ds->name,"__LINKEDIT")){if(memcmp(packed,srcseg,n))linkm++;}
      else if(!strcmp(ds->name,"__TEXT")){uint64_t he=sizeof(struct mh64)+sh->sizeofcmds;if(n>he&&memcmp(packed+he,srcseg+he,n-he))textbodym++;}
      checked++;
    }
    munmap(src,s2.st_size);
  }
  printf("byte-identity: DATA mismatch=%d LINKEDIT mismatch=%d TEXT-body mismatch=%d (segs=%d)\n",datam,linkm,textbodym,checked);
  return (datam||linkm||textbodym)?1:0;
}
