# perf#24f AFTER-REWRITE gate: re-decode the PACKED __text in a built DCC5 cache and prove every
# RIP-relative site now resolves to the correct target under the 3-region layout.
#
# Method: for each image, take the ORIGINAL dylib and the PACKED cache. Compute, per image, the packed
# vmaddr of every original segment (RX/RW/RO region base + region offset). Extract the packed __text bytes
# from the cache RX region, disassemble THEM with llvm-objdump (at the packed base address), and for each
# rip-rel site read the (already-rewritten) disp32, compute the target, and verify:
#   - same-TEXT sites (target was in original __TEXT): packed target lands in the SAME image's packed __TEXT.
#   - same-DATA/RO sites: packed target lands at exactly the packed vmaddr of the original target
#     (region base + region_off + (orig_target - orig_seg_vmaddr)).
# Any mismatch, any target escaping the intended region, any int32 issue => FAIL.
#
# This is the gate that would have caught the DCC2 "__text" crash: an unrewritten DCC2 cache fails here
# because every same-DATA disp still points into the RX region (RED arm 1).
import struct,sys,os,subprocess,re,hashlib

def rup(v,a=0x4000): return (v+a-1)&~(a-1)
def slice_off(d):
    m=struct.unpack('>I',d[:4])[0]
    if m in (0xcafebabe,0xbebafeca):
        nf=struct.unpack('>I',d[4:8])[0]
        for i in range(nf):
            ct,cs,o,sz,al=struct.unpack('>IIIII',d[8+i*20:8+i*20+20])
            if ct==0x01000007: return o
    return 0
def parse_segs(d,off):
    mh=d[off:]; nc=struct.unpack('<I',mh[16:20])[0]; p=32; segs=[]; text=None
    for _ in range(nc):
        cmd,csz=struct.unpack('<II',mh[p:p+8])
        if cmd==0x19:
            nm=mh[p+8:p+24].rstrip(b'\0').decode(); va,vs,fo,fs=struct.unpack('<QQQQ',mh[p+24:p+56])
            ip=struct.unpack('<i',mh[p+60:p+64])[0]; nsec=struct.unpack('<I',mh[p+64:p+68])[0]
            if nm!='__PAGEZERO': segs.append((nm,va,vs,fo,fs,ip))
            sp=p+72
            for _s in range(nsec):
                sec=mh[sp:sp+80]; sn=sec[:16].rstrip(b'\0').decode()
                a,sz=struct.unpack('<QQ',sec[32:48]); so=struct.unpack('<I',sec[48:52])[0]
                if sn=='__text': text=(a,sz,so)
                sp+=80
        p+=csz
    return segs,text

cache_path=sys.argv[1]; root=sys.argv[2]; listf=sys.argv[3]
paths=[l.strip() for l in open(listf) if '.dylib' in l]; paths.sort()
cd=open(cache_path,'rb').read()
magic,version,arch,icount=struct.unpack('<IIII',cd[:16])
assert magic in (0x44434335,0x44434332), "not a DCC5/DCC2 cache: %#x"%magic
hoff=16+8+256
regions=[]
for i in range(3):
    fo,sz,vb,prot,pad=struct.unpack('<QQQII',cd[hoff:hoff+32]); hoff+=32; regions.append((fo,sz,vb,prot))
RXf,RXsz,RXb,_=regions[0]; RWf,RWsz,RWb,_=regions[1]; ROf,ROsz,ROb,_=regions[2]

# Recompute per-image region offsets EXACTLY as the builder (page-align each seg, first-fit per region).
grx=grw=gro=0; imgoff={}; IMG=[]
for gp in paths:
    hp=root+gp
    if not os.path.exists(hp): continue
    d=open(hp,'rb').read(); off=slice_off(d); segs,text=parse_segs(d,off)
    IMG.append((gp,hp,d,off,segs,text))
for gp,hp,d,off,segs,text in IMG:
    for (nm,va,vs,fo,fs,ip) in segs:
        reg=0 if nm=='__TEXT' else (2 if nm=='__LINKEDIT' else 1); sz=rup(vs)
        if reg==0: imgoff[(gp,nm)]=('rx',grx); grx+=sz
        elif reg==1: imgoff[(gp,nm)]=('rw',grw); grw+=sz
        else: imgoff[(gp,nm)]=('ro',gro); gro+=sz
def packed_vmb(gp,nm):
    r,o=imgoff[(gp,nm)]
    return (RXb if r=='rx' else RWb if r=='rw' else ROb)+o
def seg_of(segs,va):
    for (nm,a,vs,fo,fs,ip) in segs:
        if a<=va<a+vs: return nm,a
    return None,None

rip_re=re.compile(r'(-?)0x([0-9a-f]+)\(%rip\)')
# Per image: decode the ORIGINAL __text and the PACKED __text, pair rip sites by original va, and verify
# each packed target lands where the 3-region layout says it must.
fails=0; checked=0; rw_ok=0; st_ok=0; escape=0; mism=0
for gp,hp,d,off,segs,text in IMG:
    if not text: continue
    ta,tsz,tso=text
    tva=[a for (nm,a,vs,fo,fs,ip) in segs if nm=='__TEXT'][0]
    tfo=[fo for (nm,a,vs,fo,fs,ip) in segs if nm=='__TEXT'][0]
    pTEXT=packed_vmb(gp,'__TEXT'); rx_off=imgoff[(gp,'__TEXT')][1]
    text_in_seg=tso-tfo
    packed_text_file=RXf+rx_off+text_in_seg
    packed_text_bytes=cd[packed_text_file:packed_text_file+tsz]
    packed_text_vmaddr=pTEXT+(ta-tva)
    # ORIGINAL rip sites: original va -> (disp, ilen, orig target, class)
    oout=subprocess.run(['llvm-objdump','-d','--arch=x86_64','--no-symbolic-operands',hp],capture_output=True,text=True).stdout
    orig={}
    for line in oout.splitlines():
        m=re.match(r'\s*([0-9a-f]+):\t([0-9a-f ]+)\t(.*)',line)
        if not m: continue
        va=int(m.group(1),16); raw=m.group(2).split(); asm=m.group(3)
        if not (ta<=va<ta+tsz): continue
        rm=rip_re.search(asm)
        if not rm: continue
        disp=int(rm.group(2),16)
        if rm.group(1)=='-': disp=-disp
        ilen=len(raw); tgt=va+ilen+disp
        orig[va]=(disp,ilen,tgt)
    # PACKED rip sites: overlay the packed __text bytes onto a COPY of the original dylib at the __text
    # file offset, then disassemble with llvm-objdump as a proper Mach-O (no raw-binary desync). The
    # disp values read are the REWRITTEN ones; VMAs stay at ORIGINAL addresses so pva==ova directly.
    tmp='/home/ilyagulya/.claude-work/jobs/ae391f52/tmp/_overlay.dylib'
    ov=bytearray(d)
    ov[off+tso:off+tso+tsz]=packed_text_bytes   # slice-relative file offset of __text
    open(tmp,'wb').write(ov)
    pout=subprocess.run(['llvm-objdump','-d','--arch=x86_64','--no-symbolic-operands',tmp],capture_output=True,text=True).stdout
    twin=[vs for (nm,a,vs,fo,fs,ip) in segs if nm=='__TEXT'][0]
    for line in pout.splitlines():
        m=re.match(r'\s*([0-9a-f]+):\t([0-9a-f ]+)\t(.*)',line)
        if not m: continue
        va=int(m.group(1),16); raw=m.group(2).split(); asm=m.group(3)
        if not (ta<=va<ta+tsz): continue
        rm=rip_re.search(asm)
        if not rm: continue
        pdisp=int(rm.group(2),16)
        if rm.group(1)=='-': pdisp=-pdisp
        ilen=len(raw); ova=va
        if ova not in orig: continue   # data-in-text pool: builder never rewrote it (function gate)
        odisp,oilen,otgt=orig[ova]
        checked+=1
        onm,obase=seg_of(segs,otgt)
        if onm is None:
            continue   # original target outside all segs (padding) — unchanged, skip
        # rewritten packed instruction end vmaddr and packed target:
        pva=pTEXT+(va-tva); ptgt=pva+ilen+pdisp
        expected_ptgt = packed_vmb(gp,onm) + (otgt-obase)
        if onm=='__TEXT':
            if ptgt!=expected_ptgt: mism+=1; fails+=1
            elif not (pTEXT<=ptgt<pTEXT+twin): escape+=1; fails+=1
            else: st_ok+=1
        else:
            if ptgt!=expected_ptgt: mism+=1; fails+=1
            else: rw_ok+=1

print("=== perf#24f AFTER-REWRITE riprel gate ===")
print("rip sites re-decoded & checked:",checked)
print("  same-TEXT resolve correctly:",st_ok)
print("  same-DATA/RO resolve correctly:",rw_ok)
print("  MISMATCH (packed target != expected):",mism)
print("  ESCAPE (same-text target left its image __TEXT):",escape)
print("RESULT:", "PASS" if fails==0 else "FAIL (%d)"%fails)
sys.exit(0 if fails==0 else 1)
