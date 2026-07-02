# perf#24c2d-callsite ROOT-CAUSE PROOF (found 2026-07-02, Opus).
# Scan an image's __stubs in the DCC2 RX region for `ff 25 disp32` (jmp *disp(%rip)); compute the
# RIP-relative target and flag any that lands in an image header/section-table zone.
#
# THE BUG: DCC2 rewrites LC_SEGMENT_64.vmaddr + section_64.addr region-relative, moving __TEXT (RX)
# and __DATA (RW, where __la_symbol_ptr/__got live) FAR apart. The disp32 immediate baked into each
# stub was computed for the ORIGINAL (close) TEXT->DATA spacing and is NOT a pointer slot, so the
# DCC2 fixup table cannot touch it. Every stub therefore jumps through the WRONG address (the next
# packed RX image's header). When a stub target == some image_vmbase+0x68 (first section_64.sectname
# = "__text"), the jump target is 0x747865745f5f => the captured instruction-fetch SIGSEGV.
# This breaks EVERY RIP-relative TEXT->DATA reference, not just stubs.
#
# Prep: dcc2-extractrx <cache> rx.bin   (RX vm_base is 0, so file off in rx.bin == region-rel addr).
# The vmbases list below = each image's __TEXT region-relative vmaddr (from dcc2-inspect / findtext).
import struct
rx=open('rx.bin','rb').read()
# libc++ __stubs at 0x65e80 size 0x366 (region-rel absolute == file offset in rx.bin since RX vm_base=0)
start=0x65e80; size=0x366
# image vmbases (from findtext: image+0x68 hits). collect all +0x68 targets:
vmbases=[0x0,0x8000,0x70000,0x90000,0xcc000,0x120000,0x144000,0x15c000,0x160000,0x170000,0x178000,0x184000,0x1a4000,0x228000,0x288000,0x28c000,0x298000,0x2a0000,0x2a4000,0x2a8000,0x2d0000,0x2d4000,0x39c000,0x3a0000,0x3a4000,0x3c0000,0x3cc000,0x3d0000,0x3d4000,0x40c000,0x46c000,0x49c000,0x4d0000,0x4d4000,0x4e0000,0x4e8000,0x4f4000,0x4f8000,0x500000,0x508000]
text68=set(b+0x68 for b in vmbases)
o=start
while o < start+size:
    if rx[o]==0xff and rx[o+1]==0x25:
        disp=struct.unpack('<i',rx[o+2:o+6])[0]
        tgt=o+6+disp   # rip = next instruction
        flag=''
        if tgt in text68: flag=' <<< HITS image+0x68 (__text sectname) !!!'
        # also flag if lands in any header zone (<0x1000 into an image after subtracting nearest base)
        print(f"stub@0x{o:x} disp=0x{disp:x} -> 0x{tgt:x}{flag}")
        o+=6
    else:
        o+=1
