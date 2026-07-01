#!/bin/bash
set -u
CC=/Library/Developer/CommandLineTools/usr/bin/clang
SDK=/Library/Developer/CommandLineTools/SDKs/MacOSX11.sdk
D=/Users/ilyagulya/d17rr-chunk
N=${N:-200}; CHUNK=${CHUNK:-25}
rm -rf "$D"; mkdir -p "$D"; cd "$D" || exit 3
i=1; while [ $i -le $N ]; do
  printf "int z%s(int x){int s=x;for(int i=0;i<400;i++)s+=i*x*x+%s;return s;}\n" $i $i > f$i.c
  i=$((i+1))
done
# launch chunks in parallel: each chunk = one driver call over CHUNK sources
i=1
while [ $i -le $N ]; do
  SRC=""; j=0
  while [ $j -lt $CHUNK ] && [ $i -le $N ]; do SRC="$SRC f$i.c"; i=$((i+1)); j=$((j+1)); done
  ( cd "$D" && $CC -isysroot "$SDK" -O2 -c $SRC ) &
done
wait
echo "[chunk] built $(ls *.o 2>/dev/null | wc -l | tr -d ' ')/$N objs (chunk=$CHUNK)"
