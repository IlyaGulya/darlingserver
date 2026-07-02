#!/bin/bash
# perf#24f — consolidated OFFLINE gate suite for the DCC5 (3-region + TEXT rip-rel rewrite) cache.
# Runs EVERY hard gate + all 5 RED arms. Exit 0 only if every gate is green. This must be GREEN before
# any deploy / live boot (task #100 hard requirement).
#
# usage: dcc5-gates.sh <install_root> <closure_list.txt> <workdir>
set -u
ROOT="${1:?install_root}"; LIST="${2:?closure_list}"; WORK="${3:?workdir}"
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="$WORK/dcc5-builder"; GATE="$HERE/dcc5-riprelgate.py"; RED="$HERE/dcc5-redarms.py"
CACHE="$WORK/full.dcc5"; DCC2BIN="$WORK/dcc2-builder"; DCC2CACHE="$WORK/full.dcc2"
fail=0; pass(){ echo "  PASS  $*"; }; bad(){ echo "  FAIL  $*"; fail=1; }

echo "== build DCC5 =="
gcc -O2 -o "$BIN" "$HERE/dcc5-builder.c" 2>/dev/null || { echo "compile failed"; exit 2; }
"$BIN" "$ROOT" "$LIST" "$CACHE" > "$WORK/build.log" 2>&1 || { echo "build failed"; cat "$WORK/build.log"; exit 2; }
grep -q 'int32-overflow=0 outside-seg=0 pool-overlap=0' "$WORK/build.log" && pass "GATE3 no unresolved classes (int32=0 outside=0 pool-overlap=0)" || bad "GATE3 unresolved classes"
grep -q 'UNRESOLVED (suspicious)=0' "$WORK/build.log" && pass "fixups: 0 suspicious unresolved" || bad "suspicious unresolved binds"
grep -q 'constant-pool (data-in-text) bytes protected=24' "$WORK/build.log" && pass "GATE1 libsystem_m 24 pool bytes protected" || bad "GATE1 pool byte count != 24"

echo "== GATE2/4 after-rewrite riprel gate (every same-DATA rewritten, every same-TEXT unchanged) =="
python3 "$GATE" "$CACHE" "$ROOT" "$LIST" > "$WORK/gate.log" 2>&1
if [ $? -eq 0 ] && grep -q 'RESULT: PASS' "$WORK/gate.log"; then pass "GATE2 rewrite coverage complete (0 mismatch/escape)"; else bad "GATE2 rewrite coverage"; cat "$WORK/gate.log"; fi

echo "== GATE5 determinism (md5-stable across rebuilds) =="
"$BIN" "$ROOT" "$LIST" "$WORK/_det.dcc5" 2>/dev/null
if [ "$(md5sum < "$CACHE")" = "$(md5sum < "$WORK/_det.dcc5")" ]; then pass "GATE5 deterministic md5"; else bad "GATE5 non-deterministic"; fi
rm -f "$WORK/_det.dcc5"

echo "== RED ARM 1: unrewritten DCC2 must FAIL the rewrite gate =="
gcc -O2 -o "$DCC2BIN" "$HERE/dcc2-builder.c" 2>/dev/null && "$DCC2BIN" "$ROOT" "$LIST" "$DCC2CACHE" >/dev/null 2>&1
python3 "$GATE" "$DCC2CACHE" "$ROOT" "$LIST" >/dev/null 2>&1
[ $? -eq 1 ] && pass "RED1 unrewritten DCC2 detected (gate FAIL)" || bad "RED1 unrewritten DCC2 slipped through"

echo "== RED ARMS 2/3/4: skipped rewrite / wrong disp / same-TEXT-as-data =="
python3 "$RED" "$CACHE" "$ROOT" "$LIST" > "$WORK/red.log" 2>&1
if [ $? -eq 0 ] && grep -q 'RED ARMS: ALL PASS' "$WORK/red.log"; then pass "RED2/3/4 all detected"; else bad "RED2/3/4"; cat "$WORK/red.log"; fi

echo "== RED ARM 5: pool treated as code (--red5) must ABORT =="
"$BIN" --red5 "$ROOT" "$LIST" "$WORK/_red5.dcc5" >/dev/null 2>&1
[ $? -eq 1 ] && pass "RED5 pool-as-code detected (build ABORT)" || bad "RED5 pool-as-code slipped through"
rm -f "$WORK/_red5.dcc5" "$WORK/_red5.dcc5.tmp"

echo
if [ $fail -eq 0 ]; then echo "==== ALL OFFLINE GATES GREEN — DCC5 cache safe to proceed to LIVE sequence ===="; else echo "==== GATES FAILED — DO NOT DEPLOY ===="; fi
exit $fail
