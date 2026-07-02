# True coverage of __text ONLY (the arbitrary-instruction section) via objdump; stubs/stub_helper are
# uniform and handled separately. Report undecoded bytes strictly within each image's __text.
import struct,sys,os,subprocess,re
def slice_off(d):
    m=struct.unpack('>I',d[:4])[0]
    if m in (0xcafebabe,0xbebafeca):
        nf=struct.unpack('>I',d[4:8])[0]
        for i in range(nf):
            ct,cs,o,sz,al=struct.unpack('>IIIII',d[8+i*20:8+i*20+20])
            if ct==0x01000007:return o
    return 0
def parse(d,off):
    mh=d[off:];nc=struct.unpack('<I',mh[16:20])[0];p=32;sects=[]
    for _ in range(nc):
        cmd,csz=struct.unpack('<II',mh[p:p+8])
        if cmd==0x19:
            nsec=struct.unpack('<I',mh[p+64:p+68])[0];sp=p+72
            for _s in range(nsec):
                sec=mh[sp:sp+80];sn=sec[:16].rstrip(b'\0').decode();sg=sec[16:32].rstrip(b'\0').decode()
                a,sz=struct.unpack('<QQ',sec[32:48]);so=struct.unpack('<I',sec[48:52])[0]
                fl=struct.unpack('<I',sec[64:68])[0];sects.append((sg,sn,a,sz,so,fl));sp+=80
        p+=csz
    return sects
root=sys.argv[1];paths=[l.strip() for l in open(sys.argv[2]) if '.dylib' in l];paths.sort()
from collections import Counter
worst=[]
for gp in paths:
    hp=root+gp
    if not os.path.exists(hp):continue
    d=open(hp,'rb').read();off=slice_off(d);sects=parse(d,off)
    txt=[(a,sz,so) for (sg,sn,a,sz,so,fl) in sects if sn=='__text']
    if not txt:continue
    ta,tsz,tso=txt[0]
    covered={}
    out=subprocess.run(['llvm-objdump','-d','--arch=x86_64','--no-symbolic-operands',hp],capture_output=True,text=True).stdout
    for line in out.splitlines():
        m=re.match(r'\s*([0-9a-f]+):\t([0-9a-f ]+)\t',line)
        if not m: continue
        va=int(m.group(1),16); n=len(m.group(2).split())
        if ta<=va<ta+tsz: covered[va]=n
    pos=ta; gaps=0; nonpad=0
    while pos<ta+tsz:
        if pos in covered and covered[pos]>0: pos+=covered[pos]
        else:
            b=d[off+tso+(pos-ta)]
            if b not in (0xcc,0x00,0x90): nonpad+=1
            gaps+=1; pos+=1
    if gaps: worst.append((gp.split('/')[-1],gaps,nonpad,tsz))
print("__text coverage gaps per image (name, gapbytes, non-padding-gap, textsize):")
for w in sorted(worst,key=lambda x:-x[2])[:15]: print("  ",w)
print("images with non-padding __text gaps:",sum(1 for w in worst if w[2]>0),"of",len(paths))
print("total non-padding __text gap bytes:",sum(w[2] for w in worst))
