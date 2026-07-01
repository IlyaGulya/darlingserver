#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#define PAGE 4096
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
/* map NVMA regions totalling ~9.4MB faulted, then unmap; report unmap ms. iters procs. */
static double bench(int nvma,int iters){
  size_t total=9600*1024; size_t per=(total/nvma)&~(PAGE-1); if(per<PAGE)per=PAGE;
  double tu=0;
  for(int it=0;it<iters;it++){
    void**m=malloc(sizeof(void*)*nvma);
    for(int i=0;i<nvma;i++){ m[i]=mmap(NULL,per,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
      for(size_t o=0;o<per;o+=PAGE) ((char*)m[i])[o]=1; }
    double t0=now(); for(int i=0;i<nvma;i++) munmap(m[i],per); tu+=now()-t0;
    free(m);
  }
  return tu*1e3;
}
int main(){
  int iters=400;
  printf("nvma | unmap ms (400 iters) | us/proc | us/vma\n");
  int cases[]={6,40,120,240};
  for(int c=0;c<4;c++){ int n=cases[c]; double ms=bench(n,iters);
    printf("%4d | %8.1f | %7.2f | %6.3f\n",n,ms,ms*1000/iters,ms*1000/iters/n); }
  return 0;
}
