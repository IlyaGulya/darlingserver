/* extractrx <cache> <out.bin>: dump the whole RX region raw so we can objdump it.
 * RX region vm_base is the load base; file offset 0 in out.bin == vm_base. */
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "/home/ilyagulya/work/darling-dev/darling/src/external/darlingserver/tools/closure-cache/dcc2-format.h"
int main(int c,char**v){
 int fd=open(v[1],O_RDONLY);struct stat s;fstat(fd,&s);
 uint8_t*m=mmap(0,s.st_size,PROT_READ,MAP_PRIVATE,fd,0);
 struct dcc_header*h=(void*)m;
 FILE*o=fopen(v[2],"wb");
 fwrite(m+h->regions[0].file_off,1,h->regions[0].size,o); fclose(o);
 fprintf(stderr,"RX vm_base=0x%llx size=0x%llx written\n",(unsigned long long)h->regions[0].vm_base,(unsigned long long)h->regions[0].size);
 return 0;
}
