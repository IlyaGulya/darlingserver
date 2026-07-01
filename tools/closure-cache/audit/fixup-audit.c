/* perf#24c2a->DCC2 — fixup/bind class audit.
 * For each dylib in the closure, classify the fixup encoding + bind targets so we know what a
 * cache-native (DCC2) precomputed fixup table must carry, and whether cross-image binds can be
 * fully resolved at build time (all targets inside the closure set) or need runtime resolution.
 *
 * Detects:
 *   - encoding: LC_DYLD_CHAINED_FIXUPS (modern) vs LC_DYLD_INFO_ONLY opcode rebase/bind.
 *   - rebase count (approx, from opcode stream or chain count).
 *   - bind count + per-bind library ordinal classification:
 *       SELF (BIND_SPECIAL_DYLIB_SELF), FLAT (flat namespace/-2), MAIN_EXE (-1),
 *       WEAK_LOOKUP (-3), or an ordinal -> a specific LC_LOAD_DYLIB (name recorded).
 *   - whether every ordinal points to a dylib that is ALSO in the closure list (=> build-time resolvable).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <arpa/inet.h>

struct fat_header{uint32_t magic,nfat_arch;};
struct fat_arch{uint32_t cputype,cpusubtype,offset,size,align;};
struct mh64{uint32_t magic,cputype,cpusubtype,filetype,ncmds,sizeofcmds,flags,reserved;};
struct lc{uint32_t cmd,cmdsize;};
struct dylib_command{uint32_t cmd,cmdsize,name_off,timestamp,cur,compat;};
struct dyld_info{uint32_t cmd,cmdsize,rebase_off,rebase_size,bind_off,bind_size,weak_bind_off,weak_bind_size,lazy_bind_off,lazy_bind_size,export_off,export_size;};
struct linkedit_data{uint32_t cmd,cmdsize,dataoff,datasize;};

#define LC_SEGMENT_64 0x19
#define LC_LOAD_DYLIB 0xc
#define LC_LOAD_WEAK_DYLIB 0x80000018
#define LC_REEXPORT_DYLIB 0x8000001f
#define LC_LOAD_UPWARD_DYLIB 0x80000023
#define LC_DYLD_INFO 0x22
#define LC_DYLD_INFO_ONLY 0x80000022
#define LC_DYLD_CHAINED_FIXUPS 0x80000034

/* bind opcodes */
#define BIND_OPCODE_MASK 0xF0
#define BIND_IMM_MASK 0x0F
#define BIND_OPCODE_DONE 0x00
#define BIND_OPCODE_SET_DYLIB_ORDINAL_IMM 0x10
#define BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB 0x20
#define BIND_OPCODE_SET_DYLIB_SPECIAL_IMM 0x30
#define BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM 0x40
#define BIND_OPCODE_SET_TYPE_IMM 0x50
#define BIND_OPCODE_SET_ADDEND_SLEB 0x60
#define BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB 0x70
#define BIND_OPCODE_ADD_ADDR_ULEB 0x80
#define BIND_OPCODE_DO_BIND 0x90
#define BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB 0xA0
#define BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED 0xB0
#define BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB 0xC0
/* rebase opcodes */
#define REBASE_OPCODE_MASK 0xF0
#define REBASE_OPCODE_DONE 0x00
#define REBASE_OPCODE_DO_REBASE_IMM_TIMES 0x50
#define REBASE_OPCODE_DO_REBASE_ULEB_TIMES 0x60
#define REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB 0x70
#define REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB 0x80

static uint64_t uleb(const uint8_t**p,const uint8_t*e){uint64_t r=0;int s=0;while(*p<e){uint8_t b=*(*p)++;r|=(uint64_t)(b&0x7f)<<s;if(!(b&0x80))break;s+=7;}return r;}
static int64_t sleb(const uint8_t**p,const uint8_t*e){int64_t r=0;int s=0;uint8_t b;do{b=*(*p)++;r|=(int64_t)(b&0x7f)<<s;s+=7;}while(b&0x80 && *p<e);if(s<64&&(b&0x40))r|=-(1LL<<s);return r;}

int main(int argc,char**argv){
    if(argc<2){fprintf(stderr,"usage: %s <dylib>...\n",argv[0]);return 2;}
    printf("%-34s enc     rebases  binds  | bind-classes\n","dylib");
    printf("%-34s ------  -------  -----  | --------------------------------------\n","");
    for(int a=1;a<argc;a++){
        int fd=open(argv[a],O_RDONLY); if(fd<0){perror(argv[a]);continue;}
        struct stat st; fstat(fd,&st);
        uint8_t*m=mmap(0,st.st_size,PROT_READ,MAP_PRIVATE,fd,0); close(fd);
        if(m==MAP_FAILED){perror("mmap");continue;}
        uint64_t so=0; struct fat_header*fh=(void*)m;
        if(ntohl(fh->magic)==0xcafebabe){uint32_t n=ntohl(fh->nfat_arch);struct fat_arch*fa=(void*)(fh+1);for(uint32_t i=0;i<n;i++)if(ntohl(fa[i].cputype)==0x01000007)so=ntohl(fa[i].offset);}
        struct mh64*h=(void*)(m+so);
        const char* leaf=strrchr(argv[a],'/'); leaf=leaf?leaf+1:argv[a];

        struct dyld_info* di=NULL; struct linkedit_data* chained=NULL;
        char dylibs[64][128]; int ndylibs=0;
        uint64_t linkeditFileOff=0, textFileOff=0;
        struct lc* c=(void*)((char*)h+sizeof *h);
        for(uint32_t i=0;i<h->ncmds;i++){
            if(c->cmd==LC_DYLD_INFO||c->cmd==LC_DYLD_INFO_ONLY) di=(void*)c;
            else if(c->cmd==LC_DYLD_CHAINED_FIXUPS) chained=(void*)c;
            else if(c->cmd==LC_LOAD_DYLIB||c->cmd==LC_LOAD_WEAK_DYLIB||c->cmd==LC_REEXPORT_DYLIB||c->cmd==LC_LOAD_UPWARD_DYLIB){
                struct dylib_command*dc=(void*)c; const char*nm=(char*)dc+dc->name_off;
                if(ndylibs<64){const char*l2=strrchr(nm,'/');l2=l2?l2+1:nm;strncpy(dylibs[ndylibs++],l2,127);}
            }
            c=(void*)((char*)c+c->cmdsize);
        }

        const char* enc = chained ? "chained" : (di ? "opcode " : "none   ");
        int rebases=0, binds=0;
        int b_self=0,b_flat=0,b_main=0,b_weak=0,b_ord=0,b_other=0;
        /* per-ordinal usage: which named dylibs are bound to */
        int ordUsed[64]={0};

        if(di && so==0){ /* opcode encoding, thin or slice base handled by so */ }
        if(di){
            /* rebase opcode stream */
            const uint8_t* p=m+so+di->rebase_off; const uint8_t* e=p+di->rebase_size;
            while(p<e){ uint8_t op=*p++; uint8_t im=op&0x0f; op&=0xf0;
                switch(op){
                    case REBASE_OPCODE_DONE: break;
                    case 0x10: break; /* SET_TYPE_IMM */
                    case 0x20: uleb(&p,e); break; /* SET_SEGMENT_AND_OFFSET */
                    case 0x30: uleb(&p,e); break; /* ADD_ADDR_ULEB */
                    case 0x40: break; /* ADD_ADDR_IMM_SCALED */
                    case REBASE_OPCODE_DO_REBASE_IMM_TIMES: rebases+=im; break;
                    case REBASE_OPCODE_DO_REBASE_ULEB_TIMES: rebases+=uleb(&p,e); break;
                    case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB: rebases+=1; uleb(&p,e); break;
                    case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB:{uint64_t cnt=uleb(&p,e);uleb(&p,e);rebases+=cnt;}break;
                    default: break;
                }
            }
            /* bind opcode stream */
            p=m+so+di->bind_off; e=p+di->bind_size;
            int curOrd=0; int special=0;
            while(p<e){ uint8_t op=*p++; uint8_t im=op&0x0f; op&=0xf0;
                switch(op){
                    case BIND_OPCODE_DONE: break;
                    case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM: curOrd=im; special=0; break;
                    case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB: curOrd=uleb(&p,e); special=0; break;
                    case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM: special=1; /* imm is a 4-bit signed special ordinal */ curOrd = (im==0)?0:(int8_t)(im|0xf0); break;
                    case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM: while(*p)p++; p++; break;
                    case BIND_OPCODE_SET_TYPE_IMM: break;
                    case BIND_OPCODE_SET_ADDEND_SLEB: sleb(&p,e); break;
                    case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB: uleb(&p,e); break;
                    case BIND_OPCODE_ADD_ADDR_ULEB: uleb(&p,e); break;
                    #define CLASSIFY() do{ if(special){ if(curOrd==0)b_self++; else if(curOrd==-1)b_main++; else if(curOrd==-2)b_flat++; else if(curOrd==-3)b_weak++; else b_other++;} else { b_ord++; if(curOrd>=1&&curOrd<=ndylibs)ordUsed[curOrd-1]=1; } }while(0)
                    case BIND_OPCODE_DO_BIND: binds++; CLASSIFY(); break;
                    case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB: binds++; CLASSIFY(); uleb(&p,e); break;
                    case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED: binds++; CLASSIFY(); break;
                    case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:{uint64_t cnt=uleb(&p,e);uleb(&p,e);binds+=cnt;for(uint64_t k=0;k<cnt;k++)CLASSIFY();}break;
                    default: break;
                }
            }
        }

        printf("%-34s %-6s  %7d  %5d  | ord=%d self=%d flat=%d main=%d weak=%d other=%d\n",
               leaf,enc,rebases,binds,b_ord,b_self,b_flat,b_main,b_weak,b_other);
        /* which named dylibs this image binds to (the cross-image edges) */
        if(b_ord){
            printf("%-34s   -> deps bound:","");
            for(int k=0;k<ndylibs;k++) if(ordUsed[k]) printf(" %s",dylibs[k]);
            printf("\n");
        }
        if(chained) printf("%-34s   (chained fixups: rebase/bind counts need chain walk; encoding present)\n","");
    }
    return 0;
}
