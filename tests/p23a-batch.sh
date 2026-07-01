#!/bin/bash
# perf#23a Block 3 UPPER-BOUND: same 200 sources, batched into ONE clang driver call.
# SIGNAL test only — measures the ceiling of "fewer driver invocations", NOT a build-system proposal.
set -u
CC=/Library/Developer/CommandLineTools/usr/bin/clang
SDK=/Library/Developer/CommandLineTools/SDKs/MacOSX11.sdk
D=/Users/ilyagulya/d17rr-batch
N=${N:-200}
rm -rf "$D"; mkdir -p "$D"; cd "$D" || exit 3
i=1; while [ $i -le $N ]; do
  printf "int z%s(int x){int s=x;for(int i=0;i<400;i++)s+=i*x*x+%s;return s;}\n" $i $i > f$i.c
  i=$((i+1))
done
# one driver call, all sources
SRC=""; i=1; while [ $i -le $N ]; do SRC="$SRC f$i.c"; i=$((i+1)); done
S=$(date +%s)
$CC -isysroot "$SDK" -O2 -c $SRC
E=$(date +%s)
echo "[batch] built $(ls *.o 2>/dev/null | wc -l | tr -d ' ')/$N objs in $((E-S))s (single driver call)"
