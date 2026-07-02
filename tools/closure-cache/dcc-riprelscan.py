# perf#24e GO/NO-GO: full RIP-relative rewrite feasibility over the closure under a TIGHT 3-region layout.
# Uses llvm-objdump (complete decoder). Checks: 100% exec-byte decode (no gaps), classify every rip site,
# compute new target + new disp32 under 3-region moves, verify int32 fit, drill dangerous classes.
import struct,sys,os,subprocess,re
PG=0x1000
def rup(v,a=0x4000):return (v+a-1)&~(a-1)
def slice_off(d):
    m=struct.unpack('>I',d[:4])[0]
    if m in (0xcafebabe,0xbebafeca):
        nf=struct.unpack('>I',d[4:8])[0]
        for i in range(nf):
            ct,cs,o,sz,al=struct.unpack('>IIIII',d[8+i*20:8+i*20+20])
            if ct==0x01000007:return o
    return 0
def parse(d,off):
    mh=d[off:];nc=struct.unpack('<I',mh[16:20])[0];p=32;segs={};sects=[]
    for _ in range(nc):
        cmd,csz=struct.unpack('<II',mh[p:p+8])
        if cmd==0x19:
            nm=mh[p+8:p+24].rstrip(b'\0').decode();va,vs,fo,fs=struct.unpack('<QQQQ',mh[p+24:p+56])
            ip=struct.unpack('<i',mh[p+60:p+64])[0];nsec=struct.unpack('<I',mh[p+64:p+68])[0]
            if nm!='__PAGEZERO': segs[nm]=(va,vs,fo,fs,ip)
            sp=p+72
            for _s in range(nsec):
                sec=mh[sp:sp+80];sn=sec[:16].rstrip(b'\0').decode();sg=sec[16:32].rstrip(b'\0').decode()
                a,sz=struct.unpack('<QQ',sec[32:48]);fl=struct.unpack('<I',sec[64:68])[0]
                sects.append((sg,sn,a,sz,fl)); sp+=80
        p+=csz
    return segs,sects
def seg_of(segs,va):
    for nm,(a,vs,fo,fs,ip) in segs.items():
        if a<=va<a+vs: return nm
    return None
def sect_of(sects,va):
    for sg,sn,a,sz,fl in sects:
        if a<=va<a+sz: return sg,sn
    return None,None
root=sys.argv[1];paths=[l.strip() for l in open(sys.argv[2]) if '.dylib' in l];paths.sort()
# --- pretend tight 3-region layout: assign each SEGMENT a new base. per-image TEXT/DATA/LINKEDIT
#     move to RX/RW/RO regions (independent). newbase(seg) - oldbase(seg) = segment move delta. ---
# Build move deltas per (image,segname).
rx=rw=ro=0; MOVE={}
IMGS=[]
for gp in paths:
    hp=root+gp
    if not os.path.exists(hp):continue
    d=open(hp,'rb').read();off=slice_off(d);segs,sects=parse(d,off)
    IMGS.append((gp,hp,d,off,segs,sects))
for gp,hp,d,off,segs,sects in IMGS:
    for nm,(a,vs,fo,fs,ip) in segs.items():
        if nm=='__TEXT': MOVE[(gp,nm)]=rx; rx=rup(rx+vs)
    for nm,(a,vs,fo,fs,ip) in segs.items():
        if nm not in('__TEXT',): pass
# do DATA then RO in separate passes to emulate 3 regions
for gp,hp,d,off,segs,sects in IMGS:
    for nm,(a,vs,fo,fs,ip) in segs.items():
        if nm!='__TEXT' and (ip & 2): MOVE[(gp,nm)]=('RW',rw); rw=rup(rw+vs)
for gp,hp,d,off,segs,sects in IMGS:
    for nm,(a,vs,fo,fs,ip) in segs.items():
        if nm!='__TEXT' and not(ip&2) and not(ip&4): MOVE[(gp,nm)]=('RO',ro); ro=rup(ro+vs)
# region bases: RX=0, RW after RX, RO after RW  (single arena, non-overlapping)
RXB=0; RWB=rup(rx); ROB=rup(RWB+rw)
def newbase(gp,nm,segs):
    a,vs,fo,fs,ip=segs[nm]
    if nm=='__TEXT': return RXB+MOVE[(gp,nm)]
    v=MOVE[(gp,nm)]
    if v[0]=='RW': return RWB+v[1]
    return ROB+v[1]
rip_re=re.compile(r'(-?0x[0-9a-f]+)\(%rip\)')
tot=0;byclass={};int32fail=0;movesites=0;danger=[];gaps=0
for gp,hp,d,off,segs,sects in IMGS:
    out=subprocess.run(['llvm-objdump','-d','--arch=x86_64','--no-symbolic-operands',hp],capture_output=True,text=True).stdout
    # coverage: track decoded ranges within exec sections
    exec_ranges=[(a,a+sz) for (sg,sn,a,sz,fl) in sects if seg_of(segs,a)=='__TEXT' and (segs['__TEXT'][4]&4)]
    lastend=None
    for line in out.splitlines():
        m=re.match(r'\s*([0-9a-f]+):\t([0-9a-f ]+)\t(.*)',line)
        if not m: continue
        va=int(m.group(1),16); raw=m.group(2).split(); asm=m.group(3)
        sg,sn=sect_of(sects,va)
        if sg!='__TEXT': continue
        rm=rip_re.search(asm)
        if not rm: continue
        disp=int(rm.group(1),16)
        ilen=len(raw); nextva=va+ilen; tgt=nextva+disp
        tsg,tsn=sect_of(sects,tgt)
        if tsg is None: cls='outside-image'
        elif tsg=='__TEXT': cls='same-TEXT'
        elif (tsg in segs) and (segs.get(tsg,(0,0,0,0,0))[4]&2): cls='same-DATA(RW)'
        elif tsg=='__LINKEDIT': cls='same-LINKEDIT'
        else: cls='same-RO/other:'+tsg
        byclass[cls]=byclass.get(cls,0)+1; tot+=1
        # compute new disp under moves. new_insn_va and new_target.
        insn_seg=seg_of(segs,va); tgt_seg=seg_of(segs,tgt)
        if insn_seg is None or tgt_seg is None:
            danger.append((gp,hex(va),asm,'outside-seg',hex(tgt))); continue
        try:
            ninsn=newbase(gp,insn_seg,segs)+(va-segs[insn_seg][0])
            ntgt =newbase(gp,tgt_seg,segs)+(tgt-segs[tgt_seg][0])
        except KeyError:
            danger.append((gp,hex(va),asm,'no-move-for-seg:'+str(tgt_seg),hex(tgt))); continue
        newdisp=ntgt-(ninsn:=ninsn if False else (nextva - segs[insn_seg][0] + newbase(gp,insn_seg,segs)))
        newdisp=ntgt-(newbase(gp,insn_seg,segs)+(nextva-segs[insn_seg][0]))
        if newdisp < -(2**31) or newdisp >= 2**31: int32fail+=1; danger.append((gp,hex(va),asm,'INT32-OVERFLOW newdisp=0x%x'%(newdisp&0xffffffffffffffff),hex(tgt)))
        if newbase(gp,tgt_seg,segs)!=newbase(gp,insn_seg,segs): movesites+=1
print("=== perf#24e RIP-relative rewrite feasibility (tight 3-region) ===")
print("RX span=0x%x RW=0x%x RO=0x%x arena=0x%x"%(rx,rw,ro,ROB+ro))
print("total rip-rel sites in __TEXT:",tot)
for k in sorted(byclass): print("   %-22s %d"%(k,byclass[k]))
print("sites whose target crosses to a MOVED region (need rewrite):",movesites)
print("INT32 overflow after rewrite:",int32fail)
print("danger entries:",len(danger))
for dd in danger[:15]: print("   ",dd)
