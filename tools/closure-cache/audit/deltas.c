#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string.h>
#include <arpa/inet.h>
struct fat_header{uint32_t magic,nfat_arch;};
struct fat_arch{uint32_t cputype,cpusubtype,offset,size,align;};
struct mh64{uint32_t magic,cputype,cpusubtype,filetype,ncmds,sizeofcmds,flags,reserved;};
struct seg64{uint32_t cmd,cmdsize;char segname[16];uint64_t vmaddr,vmsize,fileoff,filesize;uint32_t maxprot,initprot,nsects,flags;};
int main(int c,char**v){
  for(int a=1;a<c;a++){
    int fd=open(v[a],O_RDONLY);if(fd<0){perror(v[a]);continue;}
    struct stat st;fstat(fd,&st);
    uint8_t*m=mmap(0,st.st_size,PROT_READ,MAP_PRIVATE,fd,0);close(fd);
    uint64_t so=0;struct fat_header*fh=(void*)m;
    if(ntohl(fh->magic)==0xcafebabe){uint32_t n=ntohl(fh->nfat_arch);struct fat_arch*fa=(void*)(fh+1);for(uint32_t i=0;i<n;i++)if(ntohl(fa[i].cputype)==0x01000007)so=ntohl(fa[i].offset);}
    struct mh64*h=(void*)(m+so);
    uint64_t tv=0,dv=0,lv=0,tf=0,df=0,lf=0;
    struct seg64*s=(void*)((char*)h+sizeof *h);
    for(uint32_t i=0;i<h->ncmds;i++){
      if(s->cmd==0x19){
        if(!strcmp(s->segname,"__TEXT")){tv=s->vmaddr;tf=s->fileoff;}
        else if(!strcmp(s->segname,"__DATA")){dv=s->vmaddr;df=s->fileoff;}
        else if(!strcmp(s->segname,"__LINKEDIT")){lv=s->vmaddr;lf=s->fileoff;}
      }
      s=(void*)((char*)s+s->cmdsize);
    }
    const char*leaf=strrchr(v[a],'/');leaf=leaf?leaf+1:v[a];
    printf("%-34s vmaddr: DATA-TEXT=0x%llx LINK-TEXT=0x%llx | fileoff: DATA=0x%llx LINK=0x%llx\n",
      leaf,(unsigned long long)(dv-tv),(unsigned long long)(lv-tv),(unsigned long long)df,(unsigned long long)lf);
  }
  return 0;
}
