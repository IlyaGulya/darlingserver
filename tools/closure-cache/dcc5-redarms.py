# perf#24f RED arms for the rip-rel rewrite gate. Each mutation MUST be DETECTED (gate returns FAIL /
# exit 1). If a mutation slips through (gate PASS), the gate has no teeth and the arm FAILS.
#
# Arm 1: unrewritten DCC2 cache            -> every same-DATA disp still points into RX  (run separately)
# Arm 2: revert ONE rewritten same-DATA disp32 to its original value (a "skipped rewrite")
# Arm 3: corrupt ONE rewritten disp32 by +1 (a "wrong disp")
# Arm 4: rewrite a same-TEXT site as if it were same-DATA (jump-table / TEXT-internal treated wrongly)
# Usage: python3 dcc5-redarms.py <good.dcc5> <install_root> <fullpaths.txt>
import struct,sys,os,subprocess,re,shutil,tempfile

good=sys.argv[1]; root=sys.argv[2]; listf=sys.argv[3]
gate=os.path.join(os.path.dirname(os.path.abspath(__file__)),'dcc5-riprelgate.py')
paths=[l.strip() for l in open(listf) if '.dylib' in l]; paths.sort()

def rup(v,a=0x4000): return (v+a-1)&~(a-1)
def slice_off(d):
    m=struct.unpack('>I',d[:4])[0]
    if m in (0xcafebabe,0xbebafeca):
        nf=struct.unpack('>I',d[4:8])[0]
        for i in range(nf):
            ct,cs,o,sz,al=struct.unpack('>IIIII',d[8+i*20:8+i*20+20])
            if ct==0x01000007: return o
    return 0
def parse_text(d,off):
    mh=d[off:]; nc=struct.unpack('<I',mh[16:20])[0]; p=32; tva=tfo=None; text=None
    for _ in range(nc):
        cmd,csz=struct.unpack('<II',mh[p:p+8])
        if cmd==0x19:
            nm=mh[p+8:p+24].rstrip(b'\0').decode(); va,vs,fo,fs=struct.unpack('<QQQQ',mh[p+24:p+56]); nsec=struct.unpack('<I',mh[p+64:p+68])[0]
            if nm=='__TEXT': tva=va; tfo=fo
            sp=p+72
            for _s in range(nsec):
                sec=mh[sp:sp+80]; sn=sec[:16].rstrip(b'\0').decode()
                a,sz=struct.unpack('<QQ',sec[32:48]); so=struct.unpack('<I',sec[48:52])[0]
                if sn=='__text': text=(a,sz,so)
                sp+=80
        p+=csz
    return tva,tfo,text

cd=open(good,'rb').read()
hoff=16+8+256; regions=[]
for i in range(3):
    fo,sz,vb,prot,pad=struct.unpack('<QQQII',cd[hoff:hoff+32]); hoff+=32; regions.append((fo,sz,vb))
RXf,RXsz,RXb=regions[0]

# recompute rx_off per image (page-align each TEXT seg, first-fit) — matches builder
grx=0; rxoff={}
IMG=[]
for gp in paths:
    hp=root+gp
    if not os.path.exists(hp): continue
    d=open(hp,'rb').read(); off=slice_off(d); tva,tfo,text=parse_text(d,off)
    IMG.append((gp,hp,d,off,tva,tfo,text))
# region-offset assignment needs ALL segs in order; replicate builder exactly
def all_segs(d,off):
    mh=d[off:]; nc=struct.unpack('<I',mh[16:20])[0]; p=32; segs=[]
    for _ in range(nc):
        cmd,csz=struct.unpack('<II',mh[p:p+8])
        if cmd==0x19:
            nm=mh[p+8:p+24].rstrip(b'\0').decode(); va,vs,fo,fs=struct.unpack('<QQQQ',mh[p+24:p+56])
            if nm!='__PAGEZERO': segs.append((nm,va,vs))
        p+=csz
    return segs
for gp,hp,d,off,tva,tfo,text in IMG:
    for (nm,va,vs) in all_segs(d,off):
        reg=0 if nm=='__TEXT' else (2 if nm=='__LINKEDIT' else 1); sz=rup(vs)
        if reg==0: rxoff[gp]=grx; grx+=sz
        elif reg==1: pass
        else: pass
# note: rxoff captures only TEXT region running offset; but we advanced grx only on TEXT above. Good.

# find the first image's first rip-rel same-DATA site and same-TEXT site (via original decode)
rip_re=re.compile(r'(-?)0x([0-9a-f]+)\(%rip\)')
def find_sites(gp,hp,d,off,tva,tfo,text):
    ta,tsz,tso=text
    segs=all_segs(d,off)
    def seg_of(va):
        for (nm,a,vs) in segs:
            if a<=va<a+vs: return nm
        return None
    out=subprocess.run(['llvm-objdump','-d','--arch=x86_64','--no-symbolic-operands',hp],capture_output=True,text=True).stdout
    sameDATA=None; sameTEXT=None
    for line in out.splitlines():
        m=re.match(r'\s*([0-9a-f]+):\t([0-9a-f ]+)\t(.*)',line)
        if not m: continue
        va=int(m.group(1),16); raw=m.group(2).split(); asm=m.group(3)
        if not (ta<=va<ta+tsz): continue
        rm=rip_re.search(asm)
        if not rm: continue
        disp=int(rm.group(2),16)
        if rm.group(1)=='-': disp=-disp
        ilen=len(raw); tgt=va+ilen+disp
        # cache-file offset of this instruction's __text bytes
        text_in_seg=tso-tfo
        file_off=RXf+rxoff[gp]+text_in_seg+(va-ta)
        # locate disp32 byte position within raw by unique LE match
        old32=disp & 0xffffffff
        positions=[]
        for b in range(0,ilen-3):
            v=int(raw[b],16)|(int(raw[b+1],16)<<8)|(int(raw[b+2],16)<<16)|(int(raw[b+3],16)<<24)
            if v==old32: positions.append(b)
        if len(positions)!=1: continue
        dpos=file_off+positions[0]
        tnm=seg_of(tgt)
        if tnm and tnm!='__TEXT' and sameDATA is None: sameDATA=(va,dpos,old32)
        if tnm=='__TEXT' and sameTEXT is None: sameTEXT=(va,dpos,old32)
        if sameDATA and sameTEXT: break
    return sameDATA,sameTEXT

gp,hp,d,off,tva,tfo,text=IMG[0]
sameDATA,sameTEXT=find_sites(gp,hp,d,off,tva,tfo,text)
print("first image:",gp)
print("  sameDATA site:",sameDATA,"  sameTEXT site:",sameTEXT)

def run_gate(path):
    r=subprocess.run(['python3',gate,path,root,listf],capture_output=True,text=True)
    return r.returncode, r.stdout.strip().splitlines()[-1] if r.stdout else ''

results={}

# --- Arm 2: revert a rewritten same-DATA disp32 to ORIGINAL value (skipped rewrite) ---
if sameDATA:
    va,dpos,old32=sameDATA
    b=bytearray(cd); b[dpos:dpos+4]=struct.pack('<I',old32)   # put original disp back = "not rewritten"
    tf=tempfile.mktemp(suffix='.dcc5'); open(tf,'wb').write(b)
    rc,last=run_gate(tf); os.unlink(tf)
    results['arm2 (skipped same-DATA rewrite)']=(rc==1,rc,last)

# --- Arm 3: corrupt a rewritten disp32 by +0x10 (wrong disp) ---
if sameDATA:
    va,dpos,old32=sameDATA
    cur=struct.unpack('<I',cd[dpos:dpos+4])[0]
    b=bytearray(cd); b[dpos:dpos+4]=struct.pack('<I',(cur+0x10)&0xffffffff)
    tf=tempfile.mktemp(suffix='.dcc5'); open(tf,'wb').write(b)
    rc,last=run_gate(tf); os.unlink(tf)
    results['arm3 (wrong disp32)']=(rc==1,rc,last)

# --- Arm 4: rewrite a same-TEXT site as though it targeted moved DATA (jump-table treated as data) ---
# Simulate the builder mistakenly applying a DATA-move delta to a same-TEXT disp: shift it by the
# RX->RW gap so its target escapes __TEXT. Gate must flag escape/mismatch.
if sameTEXT:
    va,dpos,old32=sameTEXT
    RWb=regions[1][2]
    b=bytearray(cd); b[dpos:dpos+4]=struct.pack('<I',(old32+ (RWb-RXb))&0xffffffff)
    tf=tempfile.mktemp(suffix='.dcc5'); open(tf,'wb').write(b)
    rc,last=run_gate(tf); os.unlink(tf)
    results['arm4 (same-TEXT/jump-table rewritten as data)']=(rc==1,rc,last)

print("\n=== RED ARM RESULTS (each must be DETECTED = gate FAIL/exit1) ===")
allok=True
for k,(ok,rc,last) in results.items():
    print("  %-46s %s  (exit=%d, %s)"%(k, "PASS(detected)" if ok else "FAIL(slipped!)", rc, last))
    allok=allok and ok
print("\nRED ARMS:", "ALL PASS" if allok else "SOME FAILED")
sys.exit(0 if allok else 1)
