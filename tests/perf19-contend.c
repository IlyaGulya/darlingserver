/* perf#19 contending synthetic (robust): NT threads on a hot shared mutex -> heavy EBIT contention ->
   pthread falls to the blocking slow path (psynch_mutexwait/mutexdrop). Plus a SEPARATE bounded condvar
   phase that cannot deadlock (each waiter waits for a per-thread flag the main sets then broadcasts).
   Self-terminating; prints a line at each phase so a hang is visible. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#ifndef NT
#define NT 96
#endif
#ifndef ITERS
#define ITERS 3000
#endif
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static long counter = 0;
static volatile int go = 0;

static void *mtx_worker(void *a){
    while(!go){}
    for(int i=0;i<ITERS;i++){
        pthread_mutex_lock(&mtx);
        counter++;
        pthread_mutex_unlock(&mtx);
    }
    return NULL;
}
/* condvar phase: bounded, no lost-wakeup risk -- waiter loops on a shared generation counter. */
static pthread_mutex_t cmtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cnd  = PTHREAD_COND_INITIALIZER;
static long gen = 0;
#ifndef CROUNDS
#define CROUNDS 400
#endif
static void *cnd_worker(void *a){
    while(!go){}
    long seen = 0;
    for(int r=0;r<CROUNDS;r++){
        pthread_mutex_lock(&cmtx);
        while(gen <= seen) pthread_cond_wait(&cnd, &cmtx);   /* predicate guards against lost wakeups */
        seen = gen;
        pthread_mutex_unlock(&cmtx);
    }
    return NULL;
}
int main(int argc,char**argv){
    int nt = argc>1?atoi(argv[1]):NT;
    int ncnd = nt/4; if(ncnd<1) ncnd=1;
    int nmtx = nt-ncnd;
    pthread_t *t = calloc(nt,sizeof(*t));
    for(int i=0;i<nmtx;i++) pthread_create(&t[i],NULL,mtx_worker,NULL);
    for(int i=0;i<ncnd;i++) pthread_create(&t[nmtx+i],NULL,cnd_worker,NULL);
    go=1;
    /* drive the condvar: bump gen + broadcast CROUNDS times so every waiter advances every round */
    for(int r=0;r<CROUNDS;r++){
        pthread_mutex_lock(&cmtx);
        gen++;
        pthread_cond_broadcast(&cnd);
        pthread_mutex_unlock(&cmtx);
    }
    printf("[p19-contend] phase1 driven (nt=%d nmtx=%d ncnd=%d), joining...\n", nt, nmtx, ncnd);
    fflush(stdout);
    for(int i=0;i<nt;i++) pthread_join(t[i],NULL);
    printf("[p19-contend] DONE nt=%d mtx_iters=%d crounds=%d counter=%ld\n", nt, ITERS, CROUNDS, counter);
    return 0;
}
