# CORRECTED: strictly non-overlapping placement (two images never share a page). Same-perm runs
# merge only by ABUTTING. Measures the HONEST perm-run VMA count.
import struct,sys,os
PG=0x1000
def rup(v,a=PG):return (v+a-1)&~(a-1)
def slice_off(d):
    m=struct.unpack('>I',d[:4])[0]
    if m in (0xcafebabe,0xbebafeca):
        nf=struct.unpack('>I',d[4:8])[0]
        for i in range(nf):
            ct,cs,o,sz,al=struct.unpack('>IIIII',d[8+i*20:8+i*20+20])
            if ct==0x01000007:return o
    return 0
def parse(d,off):
    mh=d[off:];nc=struct.unpack('<I',mh[16:20])[0];p=32;s={}
    for _ in range(nc):
        cmd,csz=struct.unpack('<II',mh[p:p+8])
        if cmd==0x19:
            nm=mh[p+8:p+24].rstrip(b'\0').decode();va,vs,fo,fs=struct.unpack('<QQQQ',mh[p+24:p+56])
            pr=struct.unpack('<i',mh[p+60:p+64])[0]
            if nm!='__PAGEZERO': s[nm]=(va,vs,pr)
        p+=csz
    return s
root=sys.argv[1];paths=[l.strip() for l in open(sys.argv[2]) if '.dylib' in l];paths.sort()
IMG=[]
for gp in paths:
    hp=root+gp
    if not os.path.exists(hp):continue
    d=open(hp,'rb').read();segs=parse(d,slice_off(d));T=segs['__TEXT']
    pages={}
    for sn,(va,vs,pr) in segs.items():
        rs=va-T[0]
        for b in range(rs,rs+rup(vs),PG): pages[b//PG]=pr
    IMG.append([gp.split('/')[-1],pages,max(pages)+1])
def runs_of(occ):
    keys=sorted(occ);runs=0;last=None;lp=None
    for k in keys:
        if occ[k]!=last or (lp is not None and k!=lp+1):runs+=1
        last=occ[k];lp=k
    return runs
def pack(order):
    occ={}
    for idx in order:
        pages=IMG[idx][1]; base=0
        while True:
            if all((base+rp) not in occ for rp in pages): break
            base+=1
        for rp,pr in pages.items(): occ[base+rp]=pr
    return occ
import random
orders={'largest':sorted(range(len(IMG)),key=lambda i:-len(IMG[i][1])),
        'as-listed':list(range(len(IMG))),
        'smallest':sorted(range(len(IMG)),key=lambda i:len(IMG[i][1]))}
best=(999,'')
for nm,o in orders.items():
    occ=pack(o);r=runs_of(occ);span=max(occ)+1
    print(f"  {nm:10} VMAs={r} span={span}pg used={len(occ)}pg fill={len(occ)/span:.0%}")
    if r<best[0]:best=(r,nm)
random.seed(3)
for t in range(50):
    o=list(range(len(IMG)));random.shuffle(o);occ=pack(o);r=runs_of(occ)
    if r<best[0]:best=(r,f'rand{t}')
print("BEST strict-nonoverlap VMAs:",best)
