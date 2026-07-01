/* perf#24b Track-1 upper-bound: raw kernel cost of two mapping patterns.
 * Models a process's dylib-closure lifecycle: map regions, fault the resident
 * portion, then munmap (the exit_mmap/zap teardown), repeated PROCS times.
 *
 * CURRENT: 40 dylibs x 3 VMAs (TEXT r-x, DATA rw, LINKEDIT r--), per-dylib sizes
 *          scaled to the real perf#24a closure (30 MB mapped, 9.4 MB faulted).
 * CACHE:   6 big regions (same total bytes / same faulted bytes), i.e. the
 *          packed shared-mapping plane (TEXT-shared, LINKEDIT-shared, DATA-COW...).
 *
 * We measure, per pattern, over PROCS iterations:
 *   - mmap wall time
 *   - fault wall time + minor faults (getrusage ru_minflt delta)
 *   - munmap (teardown) wall time
 *   - VMAs created (map_count proxy = number of mmap calls that create a VMA)
 *
 * NOTE this is an UPPER BOUND on the *mapping* win only: it isolates the VMA
 * create/fault/teardown cost from dyld bind/rebase/init (measured separately in
 * Track 2). All anonymous MAP_PRIVATE so we don't need real files; the kernel
 * VMA + pagetable + zap cost is what we're bounding, and that is file-agnostic.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <stdint.h>

#define PROCS 400          /* ~400 clang procs per build */
#define PAGE 4096

/* real per-dylib mapped/faulted KB from perf#24a ranked table (40 dylibs). */
static const int mappedKB[] = {
  21628,1216,752,872,732,484,560,1004,480,260,280,236,156,396,268,204,108,144,
  164,80,104,76,76,72,68,64,48,44,40,64,28,20,16,16,12,12,12,12,12,12
};
static const int faultedKB[] = {
  3616,676,500,496,480,432,392,372,320,244,216,204,148,136,132,112,108,100,
  92,80,76,76,72,72,68,64,48,44,40,40,28,20,16,16,12,12,12,12,12,12
};
#define NDYLIB (sizeof(mappedKB)/sizeof(mappedKB[0]))

static int prot_writable(int k){ return (k%3)==1; } /* seg k%3==1 = writable DATA */
static double now(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec+ts.tv_nsec/1e9; }
static long minflt(void){ struct rusage r; getrusage(RUSAGE_SELF,&r); return r.ru_minflt; }
static void touch(char *p, size_t bytes){ for(size_t i=0;i<bytes;i+=PAGE) p[i]=1; }

/* one "process": returns via out[] the {map,fault,unmap} seconds; vmas counted. */
static void run_current(double *tmap,double *tflt,double *tunmap,long *vmas,long *minf){
  void *maps[NDYLIB*3]; size_t szs[NDYLIB*3]; int fsz[NDYLIB*3]; int k=0;
  long mf0=minflt(); double t0=now();
  for(size_t d=0; d<NDYLIB; d++){
    size_t mapped = (size_t)mappedKB[d]*1024;
    /* split into 3 segments approximating TEXT/DATA/LINKEDIT (~55/10/35) */
    size_t seg[3]; seg[0]=mapped*55/100 & ~(PAGE-1); seg[1]=(mapped/10) & ~(PAGE-1);
    seg[2]=mapped-seg[0]-seg[1]; if(seg[1]<PAGE)seg[1]=PAGE; if((seg[2]&(PAGE-1))||seg[2]<PAGE)seg[2]=(seg[2]+PAGE-1)&~(PAGE-1);
    int prot[3]={PROT_READ|PROT_EXEC,PROT_READ|PROT_WRITE,PROT_READ};
    for(int s=0;s<3;s++){ size_t z=seg[s]?seg[s]:PAGE;
      void*p=mmap(NULL,z,prot[s],MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
      maps[k]=p; szs[k]=z; k++; }
  }
  double t1=now();
  /* fault the resident (faulted) portion of each dylib, spread across its segs */
  k=0; for(size_t d=0; d<NDYLIB; d++){ size_t fault=(size_t)faultedKB[d]*1024; size_t left=fault;
    for(int s=0;s<3;s++){ size_t z=szs[k]; size_t f=left<z?left:z; if(f && (maps[k]!=MAP_FAILED)){ if(prot_writable(k)) touch((char*)maps[k],f); else { /* r-x/r-- still fault via read */ volatile char c; for(size_t i=0;i<f;i+=PAGE) c=((char*)maps[k])[i]; (void)c; } } left-=f; k++; }
  }
  double t2=now();
  for(int i=0;i<k;i++) if(maps[i]!=MAP_FAILED) munmap(maps[i],szs[i]);
  double t3=now();
  *tmap+=t1-t0; *tflt+=t2-t1; *tunmap+=t3-t2; *vmas+=k; *minf+=minflt()-mf0;
}

static void run_cache(double *tmap,double *tflt,double *tunmap,long *vmas,long *minf){
  /* total mapped + total faulted across closure */
  size_t totmap=0, totflt=0; for(size_t d=0;d<NDYLIB;d++){ totmap+=(size_t)mappedKB[d]*1024; totflt+=(size_t)faultedKB[d]*1024; }
  /* 6 regions: TEXT-shared(RO exec), LINKEDIT-shared(RO), DATA-COW(rw), +3 slack */
  const int NREG=6; size_t reg[6]; int prot[6]={PROT_READ|PROT_EXEC,PROT_READ,PROT_READ|PROT_WRITE,PROT_READ|PROT_EXEC,PROT_READ,PROT_READ|PROT_WRITE};
  /* split totmap ~ 55/35/10 into the first 3, tiny into last 3 */
  reg[0]=(totmap*55/100)&~(PAGE-1); reg[1]=(totmap*35/100)&~(PAGE-1); reg[2]=(totmap*10/100)&~(PAGE-1);
  reg[3]=reg[4]=reg[5]=PAGE;
  void*maps[6]; long mf0=minflt(); double t0=now();
  for(int r=0;r<NREG;r++){ size_t z=reg[r]?reg[r]:PAGE; maps[r]=mmap(NULL,z,prot[r],MAP_PRIVATE|MAP_ANONYMOUS,-1,0); }
  double t1=now();
  /* fault totflt bytes, spread across the 3 big regions proportionally */
  size_t left=totflt; for(int r=0;r<3;r++){ size_t z=reg[r]; size_t f=left<z?left:z; if(maps[r]!=MAP_FAILED){ if(r==2) touch((char*)maps[r],f); else { volatile char c; for(size_t i=0;i<f;i+=PAGE) c=((char*)maps[r])[i]; (void)c; } } left-=f; }
  double t2=now();
  for(int r=0;r<NREG;r++) if(maps[r]!=MAP_FAILED) munmap(maps[r],reg[r]?reg[r]:PAGE);
  double t3=now();
  *tmap+=t1-t0; *tflt+=t2-t1; *tunmap+=t3-t2; *vmas+=NREG; *minf+=minflt()-mf0;
}

int main(int argc,char**argv){
  double cm=0,cf=0,cu=0; long cv=0,cmf=0;
  double km=0,kf=0,ku=0; long kv=0,kmf=0;
  for(int i=0;i<PROCS;i++) run_current(&cm,&cf,&cu,&cv,&cmf);
  for(int i=0;i<PROCS;i++) run_cache(&km,&kf,&ku,&kv,&kmf);
  printf("pattern    | procs | VMAs  | mmap ms | fault ms | unmap ms | total ms | minflt\n");
  printf("CURRENT 40x3| %4d | %5ld | %7.1f | %8.1f | %8.1f | %8.1f | %ld\n",PROCS,cv,cm*1e3,cf*1e3,cu*1e3,(cm+cf+cu)*1e3,cmf);
  printf("CACHE 6-reg | %4d | %5ld | %7.1f | %8.1f | %8.1f | %8.1f | %ld\n",PROCS,kv,km*1e3,kf*1e3,ku*1e3,(km+kf+ku)*1e3,kmf);
  printf("\nper-process: CURRENT %ld VMAs, CACHE %ld VMAs (%.1fx fewer)\n",cv/PROCS,kv/PROCS,(double)cv/kv);
  printf("unmap(teardown) delta: %.1f ms -> %.1f ms (%.1fx)\n",cu*1e3,ku*1e3,cu/ku);
  printf("mmap(load) delta:      %.1f ms -> %.1f ms (%.1fx)\n",cm*1e3,km*1e3,cm/km);
  printf("minflt delta:          %ld -> %ld (%.1fx)\n",cmf,kmf,(double)cmf/(kmf?kmf:1));
  return 0;
}
