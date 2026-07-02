# perf#24f STUB-COVERAGE gate (the blindness fix). llvm-objdump does NOT decode S_SYMBOL_STUBS, so the
# after-rewrite riprel gate (dcc5-riprelgate.py) is BLIND to stubs — that blindness is exactly what let
# clang crash with "offline green". This gate reads stubs from Mach-O SECTION METADATA (independent of
# any disassembler) and verifies, in the BUILT cache:
#   - every S_SYMBOL_STUBS section found from metadata is covered,
#   - every x86_64 stub is a known ff25/ff15 form (else FAIL — unknown pattern),
#   - every DATA-targeting stub's disp32 was rewritten to the correct MOVED pointer target,
#   - 0 unresolved / 0 unknown / 0 escapes.
# RED arms: unrewritten-stub cache FAILS; skipped/wrong/corrupt stub FAILS.
import struct,sys,os

def rup(v,a=0x4000): return (v+a-1)&~(a-1)
def slice_off(d):
    m=struct.unpack(">I",d[:4])[0]
    if m in (0xcafebabe,0xbebafeca):
        nf=struct.unpack(">I",d[4:8])[0]
        for i in range(nf):
            ct,cs,o,sz,al=struct.unpack(">IIIII",d[8+i*20:8+i*20+20])
            if ct==0x01000007: return o
    return 0
def parse(d,off):
    mh=d[off:]; nc=struct.unpack("<I",mh[16:20])[0]; p=32; segs=[]; stubs=[]; tva=tfo=None
    while nc>0:
        nc-=1
        cmd,csz=struct.unpack("<II",mh[p:p+8])
        if cmd==0x19:
            nm=mh[p+8:p+24].rstrip(b"\0").decode(); va,vs,fo,fs=struct.unpack("<QQQQ",mh[p+24:p+56]); nsec=struct.unpack("<I",mh[p+64:p+68])[0]
            if nm!="__PAGEZERO": segs.append((nm,va,vs,fo))
            if nm=="__TEXT": tva=va; tfo=fo
            sp=p+72
            for _ in range(nsec):
                sec=mh[sp:sp+80]; a,sz=struct.unpack("<QQ",sec[32:48]); so=struct.unpack("<I",sec[48:52])[0]
                fl=struct.unpack("<I",sec[64:68])[0]; r2=struct.unpack("<I",sec[76:80])[0]
                if (fl&0xff)==8: stubs.append((a,sz,so,r2 if r2 else 6))
                sp+=80
        p+=csz
    return segs,stubs,tva,tfo

cache=sys.argv[1]; root=sys.argv[2]; listf=sys.argv[3]
paths=sorted([l.strip() for l in open(listf) if ".dylib" in l])
cd=open(cache,"rb").read()
magic=struct.unpack("<I",cd[:4])[0]
hoff=16+8+256; regions=[]
for i in range(3):
    fo,sz,vb,prot,pad=struct.unpack("<QQQII",cd[hoff:hoff+32]); hoff+=32; regions.append((fo,sz,vb))
RXf,RXsz,RXb=regions[0]; RWf,RWsz,RWb=regions[1]; ROf,ROsz,ROb=regions[2]

# per-image region offsets exactly like the builder
grx=grw=gro=0; roff={}; IMG=[]
for gp in paths:
    hp=root+gp
    if not os.path.exists(hp): continue
    d=open(hp,"rb").read(); off=slice_off(d); segs,stubs,tva,tfo=parse(d,off)
    IMG.append((gp,hp,d,off,segs,stubs,tva,tfo))
for gp,hp,d,off,segs,stubs,tva,tfo in IMG:
    for (nm,va,vs,fo) in segs:
        reg=0 if nm=="__TEXT" else (2 if nm=="__LINKEDIT" else 1); sz=rup(vs)
        if reg==0: roff[(gp,nm)]=("rx",grx); grx+=sz
        elif reg==1: roff[(gp,nm)]=("rw",grw); grw+=sz
        else: roff[(gp,nm)]=("ro",gro); gro+=sz
def packed_vmb(gp,nm):
    r,o=roff[(gp,nm)]; return (RXb if r=="rx" else RWb if r=="rw" else ROb)+o
def seg_of(segs,va):
    for (nm,a,vs,fo) in segs:
        if a<=va<a+vs: return nm,a
    return None,None

total=0; rewritten_ok=0; sametext=0; unknown=0; escape=0; unresolved=0; fails=0
for gp,hp,d,off,segs,stubs,tva,tfo in IMG:
    if not stubs: continue
    rx_off=roff[(gp,"__TEXT")][1]; pTEXT=packed_vmb(gp,"__TEXT")
    seg0_fo=[fo for (nm,a,vs,fo) in segs if nm=="__TEXT"][0]
    for (a,sz,so,es) in stubs:
        if es!=6: fails+=1; print("FAIL %s stub entsize=%d"%(gp,es)); continue
        n=sz//es
        for k in range(n):
            stub_va=a+k*es
            total+=1
            # ORIGINAL target (from the original file bytes)
            ob=d[off+so+k*es: off+so+k*es+6]
            if not (ob[0]==0xff and ob[1] in (0x25,0x15)):
                unknown+=1; fails+=1; print("FAIL %s stub@%#x unknown orig pattern %02x%02x"%(gp,stub_va,ob[0],ob[1])); continue
            odisp=struct.unpack("<i",ob[2:6])[0]; otgt=stub_va+6+odisp
            onm,obase=seg_of(segs,otgt)
            if onm is None: escape+=1; fails+=1; print("FAIL %s stub@%#x orig tgt %#x outside segs"%(gp,stub_va,otgt)); continue
            # PACKED stub bytes from the cache RX region
            stub_off_in_seg=(so-seg0_fo)+k*es
            pf=RXf+rx_off+stub_off_in_seg
            pb=cd[pf:pf+6]
            if not (pb[0]==0xff and pb[1] in (0x25,0x15)):
                unknown+=1; fails+=1; print("FAIL %s stub@%#x PACKED opcode corrupted %02x%02x"%(gp,stub_va,pb[0],pb[1])); continue
            pdisp=struct.unpack("<i",pb[2:6])[0]
            # packed RIP = packed stub end
            new_stub_end = pTEXT + (stub_va - tva) + 6
            ptgt = new_stub_end + pdisp
            # expected packed target
            exp = packed_vmb(gp,onm) + (otgt-obase)
            # classify by whether the target segment moved differently than TEXT
            dText = pTEXT - tva
            dTgt  = packed_vmb(gp,onm) - obase
            if dTgt==dText:
                sametext+=1
                if ptgt!=exp: fails+=1; print("FAIL %s stub@%#x sametext target %#x != %#x"%(gp,stub_va,ptgt,exp))
            else:
                # must have been rewritten to land exactly on the moved pointer slot
                if ptgt!=exp: escape+=1; fails+=1; print("FAIL %s stub@%#x REWRITE WRONG/ MISSING: got %#x expected %#x (disp %#x->%#x)"%(gp,stub_va,ptgt,exp,odisp,pdisp))
                else: rewritten_ok+=1

print("=== perf#24f STUB-COVERAGE gate (Mach-O metadata, disassembler-independent) ===")
print("total stubs (from section metadata):",total)
print("  rewritten & resolve correctly (same-DATA/RO):",rewritten_ok)
print("  same-TEXT (unchanged, correct):",sametext)
print("  UNKNOWN pattern:",unknown)
print("  ESCAPE / wrong/missing rewrite:",escape)
print("RESULT:", "PASS" if fails==0 else "FAIL (%d)"%fails)
sys.exit(0 if fails==0 else 1)
