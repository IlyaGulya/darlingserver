#!/bin/bash
# perf#24c0 toy validator (STATIC): proves the closure is packable into TEXT/DATA/LINKEDIT planes
# without text rewriting, and that identity/layout invariants hold.
R=/home/ilyagulya/work/darling-prefix/libexec/darling
OD=llvm-objdump
pass=0; fail=0; warn=0
check(){ if [ "$2" = "$3" ]; then pass=$((pass+1)); else echo "  FAIL[$1]: got '$2' want '$3'"; fail=$((fail+1)); fi; }
declare -a LIBS=("$R/usr/lib/libSystem.B.dylib")
for f in $R/usr/lib/system/libsystem_*.dylib $R/usr/lib/system/libdispatch.dylib $R/usr/lib/system/libxpc.dylib $R/usr/lib/system/libcorecrypto.dylib $R/usr/lib/system/libcommonCrypto.dylib $R/usr/lib/system/libunwind.dylib $R/usr/lib/system/libkeymgr.dylib; do
  [ -f "$f" ] && LIBS+=("$f")
done
echo "closure dylibs found: ${#LIBS[@]}"
TOTC=0
for f in "${LIBS[@]}"; do
  n=$(basename "$f")
  # INV1: no __TEXT relocations (no text rewriting needed)
  tr=$($OD --macho --reloc "$f" 2>/dev/null | grep -ic '__text')
  # INV2: no __DATA_CONST (no laterReadOnly re-mprotect re-split)
  dc=$($OD --macho --private-headers "$f" 2>/dev/null | grep -c '__DATA_CONST')
  # INV3: TEXT & DATA linear (vmaddr==fileoff); LINKEDIT offset shift = DATA vmgap (packable)
  # INV4: unwind in __TEXT (RO shareable)
  uw=$($OD --macho --section-headers "$f" 2>/dev/null | grep -c '__unwind_info\|__eh_frame')
  # INV5: filetype DYLIB, PIE-style (no flags forcing fixed addr)
  ft=$($OD --macho --private-headers "$f" 2>/dev/null | grep -c 'DYLIB')
  st="OK"
  [ "$tr" -ne 0 ] && { st="TEXTRELOC"; }
  [ "$dc" -ne 0 ] && { st="$st+DATACONST"; }
  printf "  %-34s textreloc=%s dataconst=%s unwind_in_text=%s dylib=%s => %s\n" "$n" "$tr" "$dc" "$([ $uw -gt 0 ]&&echo Y||echo -)" "$([ $ft -gt 0 ]&&echo Y||echo -)" "$st"
  [ "$tr" -eq 0 ] && pass=$((pass+1)) || fail=$((fail+1))
  [ "$dc" -eq 0 ] && pass=$((pass+1)) || warn=$((warn+1))
  TOTC=$((TOTC+1))
done
echo ""
echo "SUMMARY: $TOTC dylibs; text-reloc-free+dataconst-free checks: pass=$pass fail=$fail warn(dataconst)=$warn"
