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

# ===== perf#24f STUB COVERAGE (the blindness fix) =====
# CRITICAL: the after-rewrite riprel gate above uses llvm-objdump, which does NOT decode S_SYMBOL_STUBS.
# A gate built ONLY on llvm-objdump is INSUFFICIENT (it let clang crash with "offline green"). This gate
# reads stubs from Mach-O section metadata, disassembler-independent.
STUBGATE="$HERE/dcc5-stubgate.py"
echo "== STUB GATE (Mach-O metadata): every ff25 stub covered + rewritten to moved pointer =="
python3 "$STUBGATE" "$CACHE" "$ROOT" "$LIST" > "$WORK/stub.log" 2>&1
if [ $? -eq 0 ] && grep -q 'RESULT: PASS' "$WORK/stub.log"; then
  scount=$(grep 'total stubs' "$WORK/stub.log" | grep -o '[0-9]\+' | head -1)
  pass "STUB coverage complete ($scount stubs rewritten, 0 unknown/escape)"
else bad "STUB coverage"; cat "$WORK/stub.log"; fi

echo "== STUB RED ARMS: unrewritten / skipped / wrong disp / unknown pattern must all FAIL =="
sfail=0
"$BIN" --no-stubs "$ROOT" "$LIST" "$WORK/_rs.dcc5" >/dev/null 2>&1; python3 "$STUBGATE" "$WORK/_rs.dcc5" "$ROOT" "$LIST" >/dev/null 2>&1; [ $? -eq 1 ] || { sfail=1; echo "   unrewritten-stub slipped"; }
"$BIN" --redstub=1 "$ROOT" "$LIST" "$WORK/_rs.dcc5" >/dev/null 2>&1; python3 "$STUBGATE" "$WORK/_rs.dcc5" "$ROOT" "$LIST" >/dev/null 2>&1; [ $? -eq 1 ] || { sfail=1; echo "   skip-one slipped"; }
"$BIN" --redstub=2 "$ROOT" "$LIST" "$WORK/_rs.dcc5" >/dev/null 2>&1; python3 "$STUBGATE" "$WORK/_rs.dcc5" "$ROOT" "$LIST" >/dev/null 2>&1; [ $? -eq 1 ] || { sfail=1; echo "   wrong-disp slipped"; }
"$BIN" --redstub=3 "$ROOT" "$LIST" "$WORK/_rs.dcc5" >/dev/null 2>&1; python3 "$STUBGATE" "$WORK/_rs.dcc5" "$ROOT" "$LIST" >/dev/null 2>&1; [ $? -eq 1 ] || { sfail=1; echo "   unknown-pattern slipped"; }
[ $sfail -eq 0 ] && pass "STUB RED arms all detected (unrewritten/skip/wrong/unknown)" || bad "STUB RED arms"
rm -f "$WORK/_rs.dcc5" "$WORK/_rs.dcc5.tmp"

# ===== perf#24f EXEC-COVERAGE (the general blindness fix): ZERO uncovered executable bytes =====
EXECCOV="$HERE/dcc5-execcov.py"
echo "== EXEC-COVERAGE gate: every executable byte accounted (Mach-O metadata, not objdump-only) =="
python3 "$EXECCOV" "$CACHE" "$ROOT" "$LIST" > "$WORK/exec.log" 2>&1
if [ $? -eq 0 ] && grep -q 'RESULT: PASS' "$WORK/exec.log"; then
  eb=$(grep 'total executable' "$WORK/exec.log" | grep -o '[0-9]\+')
  pass "EXEC coverage complete ($eb exec bytes, 0 uncovered)"
else bad "EXEC coverage (uncovered executable bytes)"; cat "$WORK/exec.log"; fi

echo "== EXEC RED ARMS: unrewritten __stub_helper / skipped helper rip must FAIL exec-cov =="
# NOTE the division of labor: __stubs disp correctness is the STUBGATE's job (unrewritten __stubs still
# carry valid ff25 opcodes, so exec-cov — which verifies coverage + known opcodes + helper rip targets —
# does NOT flag --no-stubs; the STUB RED arms above already prove stubgate catches it). exec-cov owns the
# __stub_helper rip targets, which the objdump-based riprel gate is blind to.
efail=0
"$BIN" --no-helper "$ROOT" "$LIST" "$WORK/_re.dcc5" >/dev/null 2>&1; python3 "$EXECCOV" "$WORK/_re.dcc5" "$ROOT" "$LIST" >/dev/null 2>&1; [ $? -eq 1 ] || { efail=1; echo "   no-helper slipped exec-cov"; }
"$BIN" --redhelper "$ROOT" "$LIST" "$WORK/_re.dcc5" >/dev/null 2>&1; python3 "$EXECCOV" "$WORK/_re.dcc5" "$ROOT" "$LIST" >/dev/null 2>&1; [ $? -eq 1 ] || { efail=1; echo "   redhelper slipped exec-cov"; }
[ $efail -eq 0 ] && pass "EXEC RED arms all detected (no-helper/redhelper)" || bad "EXEC RED arms"
rm -f "$WORK/_re.dcc5" "$WORK/_re.dcc5.tmp"

echo
if [ $fail -eq 0 ]; then echo "==== ALL OFFLINE GATES GREEN (incl. exec-coverage) ===="; else echo "==== GATES FAILED — DO NOT DEPLOY ===="; fi
exit $fail
