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
struct lc{uint32_t cmd,cmdsize;};
struct dyld_info{uint32_t cmd,cmdsize,rebase_off,rebase_size,bind_off,bind_size,weak_bind_off,weak_bind_size,lazy_bind_off,lazy_bind_size,export_off,export_size;};
static uint64_t uleb(const uint8_t**p,const uint8_t*e){uint64_t r=0;int s=0;while(*p<e){uint8_t b=*(*p)++;r|=(uint64_t)(b&0x7f)<<s;if(!(b&0x80))break;s+=7;}return r;}
static int64_t sleb(const uint8_t**p,const uint8_t*e){int64_t r=0;int s=0;uint8_t b;do{b=*(*p)++;r|=(int64_t)(b&0x7f)<<s;s+=7;}while(b&0x80&&*p<e);if(s<64&&(b&0x40))r|=-(1LL<<s);return r;}
int main(int c,char**v){
  for(int a=1;a<c;a++){
    int fd=open(v[a],O_RDONLY);struct stat st;fstat(fd,&st);uint8_t*m=mmap(0,st.st_size,PROT_READ,MAP_PRIVATE,fd,0);close(fd);
    uint64_t so=0;struct fat_header*fh=(void*)m;
    if(ntohl(fh->magic)==0xcafebabe){uint32_t n=ntohl(fh->nfat_arch);struct fat_arch*fa=(void*)(fh+1);for(uint32_t i=0;i<n;i++)if(ntohl(fa[i].cputype)==0x01000007)so=ntohl(fa[i].offset);}
    struct mh64*h=(void*)(m+so);struct lc*lc=(void*)((char*)h+sizeof *h);struct dyld_info*di=0;
    for(uint32_t i=0;i<h->ncmds;i++){if(lc->cmd==0x22||lc->cmd==0x80000022)di=(void*)lc;lc=(void*)((char*)lc+lc->cmdsize);}
    const char*leaf=strrchr(v[a],'/');leaf=leaf?leaf+1:v[a];printf("=== %s binds ===\n",leaf);
    if(!di)continue;
    const uint8_t*p=m+so+di->bind_off,*e=p+di->bind_size;int ord=0,sp=0;const char*sym="";
    while(p<e){uint8_t op=*p++;uint8_t im=op&0xf;op&=0xf0;
      switch(op){
        case 0x00:break;
        case 0x10:ord=im;sp=0;break;
        case 0x20:ord=uleb(&p,e);sp=0;break;
        case 0x30:sp=1;ord=(im==0)?0:(int8_t)(im|0xf0);break;
        case 0x40:sym=(const char*)p;while(*p)p++;p++;break;
        case 0x50:break;case 0x60:sleb(&p,e);break;case 0x70:uleb(&p,e);break;case 0x80:uleb(&p,e);break;
        case 0x90:printf("  ord=%d%s sym=%s\n",ord,sp?"(special)":"",sym);break;
        case 0xA0:printf("  ord=%d%s sym=%s\n",ord,sp?"(special)":"",sym);uleb(&p,e);break;
        case 0xB0:printf("  ord=%d%s sym=%s\n",ord,sp?"(special)":"",sym);break;
        case 0xC0:{uint64_t cnt=uleb(&p,e);uleb(&p,e);for(uint64_t k=0;k<cnt;k++)printf("  ord=%d%s sym=%s\n",ord,sp?"(special)":"",sym);}break;
      }
    }
  }
  return 0;
}
