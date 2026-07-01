import subprocess, sys, re
R="/home/ilyagulya/work/darling-prefix/libexec/darling"
libs=[f"{R}/usr/lib/libSystem.B.dylib", f"{R}/usr/lib/system/libsystem_kernel.dylib"]
def segs(f):
    out=subprocess.run(["llvm-objdump","--macho","--private-headers",f],capture_output=True,text=True).stdout
    res=[]; cur=None
    for line in out.splitlines():
        line=line.strip()
        m=re.match(r'segname (__\w+)',line)
        if m: cur={"seg":m.group(1)}
        for k in("vmaddr","vmsize","fileoff","filesize"):
            mm=re.match(rf'{k}\s+(0x[0-9a-fA-F]+|\d+)',line)
            if mm and cur is not None:
                cur[k]=int(mm.group(1),0)
                if k=="filesize":
                    if cur["seg"] in("__TEXT","__DATA","__LINKEDIT"): res.append(cur)
                    cur=None
    # dedup keep first of each seg
    seen={}; 
    for s in res:
        if s["seg"] not in seen: seen[s["seg"]]=s
    return [seen[k] for k in("__TEXT","__DATA","__LINKEDIT") if k in seen]

PAGE=0x1000
def rup(x): return (x+PAGE-1)&~(PAGE-1)
planes={"__TEXT":0,"__DATA":0,"__LINKEDIT":0}
plan=[]
for f in libs:
    s=segs(f)
    entry={"lib":f.split("/")[-1],"segs":[]}
    for seg in s:
        plane=seg["seg"]
        newoff=planes[plane]
        entry["segs"].append({"seg":plane,"old_fileoff":seg["fileoff"],"new_plane_off":newoff,
                              "filesize":seg["filesize"],"vmaddr":seg["vmaddr"],"vmsize":seg["vmsize"]})
        planes[plane]=rup(newoff+seg["filesize"])
    plan.append(entry)
print("=== PACKED PLANE LAYOUT (2-dylib toy) ===")
for e in plan:
    print(f"\n{e['lib']}:")
    for s in e["segs"]:
        print(f"  {s['seg']:11} vmaddr=0x{s['vmaddr']:x} vmsize=0x{s['vmsize']:x}  "
              f"file: 0x{s['old_fileoff']:x} -> plane+0x{s['new_plane_off']:x} (size 0x{s['filesize']:x})")
print(f"\nplane sizes: TEXT=0x{planes['__TEXT']:x}  DATA=0x{planes['__DATA']:x}  LINKEDIT=0x{planes['__LINKEDIT']:x}")
print("\n=== BUILDER FIXUP SET (per dylib, header-only — NO text/code bytes touched) ===")
print("  1. LC_SEGMENT_64.fileoff  := plane base + new_plane_off  (TEXT->text plane, DATA->data plane, LINK->link plane)")
print("  2. LC_DYLD_INFO_ONLY {rebase,bind,weak_bind,lazy_bind,export}_off  += (link_new - link_old)")
print("  3. LC_SYMTAB {symoff,stroff}  += (link_new - link_old)")
print("  4. LC_DYSYMTAB {indirectsymoff,...}  += (link_new - link_old)")
print("  5. LC_FUNCTION_STARTS / LC_DATA_IN_CODE dataoff  += (link_new - link_old)")
print("  6. LC_CODE_SIGNATURE dropped (Darling doesn't enforce) or offset-fixed")
print("  vmaddr/vmsize UNCHANGED => dladdr/dlsym/rebase/bind targets identical => IDENTITY PRESERVED")
print("  DATA plane mapped MAP_PRIVATE (COW) per process; TEXT+LINKEDIT MAP_SHARED RO")
print("\n=> VMA count: 2 dylibs x3 = 6 VMAs  ->  3 plane VMAs (TEXT/DATA/LINK). At 40 dylibs: 120 -> 3.")
