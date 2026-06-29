#!/usr/bin/env bash
# perf#18 (dar-dar6x4-perf-5dq.30) RED->GREEN gate for the ring control-block trust-boundary
# validator. Hermetic: header-only, host g++, no boot/sockets.
#
#   RED  arm: compile the test against an accept-all stub validator (-DRING_VALIDATE_STUB).
#            Every adversarial case must FAIL (stub accepts malformed input) -> the test is real.
#   GREEN arm: compile against the real dserver_ring_shm_validate(); all checks pass.
#
# Exit 0 only if RED fails (as it must) AND GREEN passes. Run from anywhere.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
INC="$(cd "$HERE/../include" && pwd)"
SRC="$HERE/ring_shm_validate_test.cpp"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

CXX="${CXX:-g++}"
CC="${CC:-gcc}"

echo "== RED arm (accept-all stub: adversarial cases MUST fail) =="
if ! "$CXX" -std=c++17 -I"$INC" -DRING_VALIDATE_STUB -o "$TMP/red" "$SRC"; then
	echo "RED arm failed to COMPILE -- gate broken"; exit 2
fi
if "$TMP/red"; then
	echo "RED arm PASSED but must FAIL (stub accepts everything) -- gate not exercising the validator"; exit 1
fi
echo "  RED arm correctly failed."
echo

echo "== GREEN arm (real validator: all checks pass) =="
if ! "$CXX" -std=c++17 -I"$INC" -o "$TMP/green" "$SRC"; then
	echo "GREEN arm failed to COMPILE"; exit 2
fi
if ! "$TMP/green"; then
	echo "GREEN arm FAILED -- validator rejects/accepts incorrectly"; exit 1
fi
echo
echo "ring_shm_validate gate: RED->GREEN OK"
echo

# --- ring_attach decision (real memfd: fstat-size-check + map + validate) ---
ASRC="$HERE/ring_attach_check_test.cpp"
echo "== attach-check RED arm (accept-all stub: adversarial cases MUST fail) =="
if ! "$CXX" -std=c++17 -D_GNU_SOURCE -I"$INC" -DRING_ATTACH_STUB -o "$TMP/ared" "$ASRC"; then
	echo "attach RED arm failed to COMPILE -- gate broken"; exit 2
fi
if "$TMP/ared"; then
	echo "attach RED arm PASSED but must FAIL -- gate not exercising the attach-check"; exit 1
fi
echo "  attach RED arm correctly failed."
echo
echo "== attach-check GREEN arm (real attach-check on real memfds) =="
if ! "$CXX" -std=c++17 -D_GNU_SOURCE -I"$INC" -o "$TMP/agreen" "$ASRC"; then
	echo "attach GREEN arm failed to COMPILE"; exit 2
fi
if ! "$TMP/agreen"; then
	echo "attach GREEN arm FAILED"; exit 1
fi
echo
echo "ring_attach_check gate: RED->GREEN OK"
echo

# --- ring datapath (P3): SPSC + server C2S service loop + reply convention, on a real
#     memfd-backed ring, with adversarial corruption + backpressure. ASan on both arms. ---
DSRC="$HERE/ring_datapath_test.cpp"
echo "== datapath GREEN arm (real SPSC + service loop, ASan) =="
if ! "$CXX" -std=c++17 -D_GNU_SOURCE -fsanitize=address -I"$INC" -o "$TMP/dp_green" "$DSRC"; then
	echo "datapath GREEN arm failed to COMPILE"; exit 2
fi
if ! "$TMP/dp_green"; then
	echo "datapath GREEN arm FAILED"; exit 1
fi
echo
echo "== datapath RED arm (accept-all stub, ASan: adversarial cases MUST fail) =="
if ! "$CXX" -std=c++17 -D_GNU_SOURCE -fsanitize=address -DRING_DATAPATH_STUB -I"$INC" -o "$TMP/dp_red" "$DSRC"; then
	echo "datapath RED arm failed to COMPILE -- gate broken"; exit 2
fi
if "$TMP/dp_red"; then
	echo "datapath RED arm PASSED but must FAIL -- gate not exercising the datapath"; exit 1
fi
echo "  datapath RED arm correctly failed."
echo
echo "ring_datapath gate: RED->GREEN OK"
echo

# --- ring eligibility ALLOWLIST (P3): the real dserver_callnum_* set that may ride the ring.
#     Needs the GENERATED rpc.h. Find it under a build dir; skip (don't fail) if not built. ---
LSRC="$HERE/ring_allowlist_test.cpp"
GEN_RPC=""
for cand in "${DSERVER_GEN_INC:-}" \
            "$HERE/../../../../build/src/external/darlingserver/include" \
            "$HOME/work/darling-build/src/external/darlingserver/include"; do
	[ -n "$cand" ] && [ -f "$cand/darlingserver/rpc.h" ] && { GEN_RPC="$cand"; break; }
done
if [ -z "$GEN_RPC" ]; then
	echo "== allowlist gate SKIPPED (generated rpc.h not found; set DSERVER_GEN_INC) =="
	echo
else
	echo "== allowlist GREEN arm (current predicate: task_self_trap + mach_reply_port) =="
	if ! "$CXX" -std=c++17 -I"$GEN_RPC" -I"$INC" -o "$TMP/al_green" "$LSRC"; then
		echo "allowlist GREEN arm failed to COMPILE"; exit 2
	fi
	if ! "$TMP/al_green"; then
		echo "allowlist GREEN arm FAILED"; exit 1
	fi
	echo
	echo "== allowlist RED arm (pre-migration predicate: task_self_trap only, MUST fail) =="
	if ! "$CXX" -std=c++17 -DALLOWLIST_OLD -I"$GEN_RPC" -I"$INC" -o "$TMP/al_red" "$LSRC"; then
		echo "allowlist RED arm failed to COMPILE -- gate broken"; exit 2
	fi
	if "$TMP/al_red"; then
		echo "allowlist RED arm PASSED but must FAIL -- gate not exercising the migration"; exit 1
	fi
	echo "  allowlist RED arm correctly failed."
	echo
	echo "ring_allowlist gate: RED->GREEN OK"
	echo

	# --- P6.1 fast-path eligibility + escape hatches (dar-ohp). Reuses $GEN_RPC. ---
	FPSRC="$HERE/ring_fastpath_gate_test.c"
	if [ -f "$FPSRC" ]; then
		echo "== fast-path GREEN arm (hatch-aware: allowlist + env kill-switches) =="
		if ! "$CC" -std=c11 -I"$GEN_RPC" -I"$INC" -o "$TMP/fp_green" "$FPSRC" 2>/dev/null; then
			echo "fast-path GREEN arm failed to COMPILE"; exit 2
		fi
		if ! "$TMP/fp_green"; then
			echo "fast-path GREEN arm FAILED"; exit 1
		fi
		echo
		echo "== fast-path RED arm (-DFASTPATH_NO_HATCH: ignores hatches, MUST fail) =="
		if ! "$CC" -std=c11 -DFASTPATH_NO_HATCH -I"$GEN_RPC" -I"$INC" -o "$TMP/fp_red" "$FPSRC" 2>/dev/null; then
			echo "fast-path RED arm failed to COMPILE -- gate broken"; exit 2
		fi
		if "$TMP/fp_red" >/dev/null 2>&1; then
			echo "fast-path RED arm PASSED but must FAIL -- gate not exercising the hatches"; exit 1
		fi
		echo "  fast-path RED arm correctly failed."
		echo
		echo "== fast-path shape RED arm (-DNO_SHAPE_GUARD: drops the direct-dispatch shape guard, MUST fail) =="
		if ! "$CC" -std=c11 -DNO_SHAPE_GUARD -I"$GEN_RPC" -I"$INC" -o "$TMP/fp_shape_red" "$FPSRC" 2>/dev/null; then
			echo "fast-path shape RED arm failed to COMPILE -- gate broken"; exit 2
		fi
		if "$TMP/fp_shape_red" >/dev/null 2>&1; then
			echo "fast-path shape RED arm PASSED but must FAIL -- gate not exercising the shape guard"; exit 1
		fi
		echo "  fast-path shape RED arm correctly failed."
		echo
		echo "== P5 mod_refs C2S allowlist RED arm (-DC2S_NO_MODREFS: drops mod_refs from the C2S allowlist, MUST fail) =="
		if ! "$CC" -std=c11 -DC2S_NO_MODREFS -I"$GEN_RPC" -I"$INC" -o "$TMP/fp_c2s_red" "$FPSRC" 2>/dev/null; then
			echo "P5 mod_refs C2S RED arm failed to COMPILE -- gate broken"; exit 2
		fi
		if "$TMP/fp_c2s_red" >/dev/null 2>&1; then
			echo "P5 mod_refs C2S RED arm PASSED but must FAIL -- gate not exercising the C2S allowlist"; exit 1
		fi
		echo "  P5 mod_refs C2S RED arm correctly failed."
		echo
		echo "ring_fastpath gate: RED->GREEN OK"
		echo
	fi

	# --- P5-bulk (dar-1il.1): no-silent-drop drift gate. Guest "may publish" set == server "will
	#     service" set (both derived from the shared DSERVER_RING_C2S_OPCODES macro). RED arm
	#     -DDRIFT_GUEST_EXTRA gives the guest an opcode the server won't service -> the gate fails. ---
	DRSRC="$HERE/ring_drift_gate_test.c"
	if [ -f "$DRSRC" ]; then
		echo "== drift GREEN arm (guest C2S set == server C2S set) =="
		if ! "$CC" -std=c11 -I"$GEN_RPC" -I"$INC" -o "$TMP/dr_green" "$DRSRC" 2>/dev/null; then
			echo "drift GREEN arm failed to COMPILE"; exit 2
		fi
		if ! "$TMP/dr_green"; then
			echo "drift GREEN arm FAILED -- guest and server C2S allowlists are not equal"; exit 1
		fi
		echo
		echo "== drift RED arm (-DDRIFT_GUEST_EXTRA: guest routes an op the server drops, MUST fail) =="
		if ! "$CC" -std=c11 -DDRIFT_GUEST_EXTRA -I"$GEN_RPC" -I"$INC" -o "$TMP/dr_red" "$DRSRC" 2>/dev/null; then
			echo "drift RED arm failed to COMPILE -- gate broken"; exit 2
		fi
		if "$TMP/dr_red" >/dev/null 2>&1; then
			echo "drift RED arm PASSED but must FAIL -- gate not exercising the no-silent-drop invariant"; exit 1
		fi
		echo "  drift RED arm correctly failed."
		echo
		echo "ring_drift gate: RED->GREEN OK"
		echo
	fi
fi

# --- P7 wake-model LOST-WAKE race gate (dar-my8). Hermetic: header-only, real shm, two threads.
#     GREEN: exhaustive ordering enumeration finds ZERO lost-wake interleavings on both the
#     request and response sides + the concurrent stress is fully live. RED arms break each
#     safety mechanism (server critical recheck / guest set-waiter-bit-before-recheck) so the
#     enumeration finds lost > 0 and the binary exits nonzero. ---
WRSRC="$HERE/ring_wake_race_test.cpp"
WR_ITERS="${RING_WAKE_RACE_ITERS:-8000}"
echo "== wake-race GREEN arm (no lost-wake interleaving; stress live) =="
if ! "$CXX" -std=c++17 -D_GNU_SOURCE -O2 -pthread -I"$INC" -o "$TMP/wr_green" "$WRSRC"; then
	echo "wake-race GREEN arm failed to COMPILE"; exit 2
fi
if ! "$TMP/wr_green" "$WR_ITERS"; then
	echo "wake-race GREEN arm FAILED -- a lost-wake invariant does not hold"; exit 1
fi
echo
echo "== wake-race RED arm A (-DRACE_NO_SERVER_RECHECK: server skips critical recheck, MUST fail) =="
if ! "$CXX" -std=c++17 -D_GNU_SOURCE -O2 -pthread -DRACE_NO_SERVER_RECHECK -I"$INC" -o "$TMP/wr_redA" "$WRSRC"; then
	echo "wake-race RED arm A failed to COMPILE -- gate broken"; exit 2
fi
if "$TMP/wr_redA" "$WR_ITERS" >/dev/null 2>&1; then
	echo "wake-race RED arm A PASSED but must FAIL -- gate not exercising the server critical recheck"; exit 1
fi
echo "  wake-race RED arm A correctly failed."
echo
echo "== wake-race RED arm B (-DRACE_WAITERBIT_AFTER_RECHECK: guest sets waiter bit too late, MUST fail) =="
if ! "$CXX" -std=c++17 -D_GNU_SOURCE -O2 -pthread -DRACE_WAITERBIT_AFTER_RECHECK -I"$INC" -o "$TMP/wr_redB" "$WRSRC"; then
	echo "wake-race RED arm B failed to COMPILE -- gate broken"; exit 2
fi
if "$TMP/wr_redB" "$WR_ITERS" >/dev/null 2>&1; then
	echo "wake-race RED arm B PASSED but must FAIL -- gate not exercising the guest waiter-bit ordering"; exit 1
fi
echo "  wake-race RED arm B correctly failed."
echo
echo "ring_wake_race gate: RED->GREEN OK"
echo

echo "all perf#18 ring gates: OK"
