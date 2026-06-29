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
fi

echo "all perf#18 ring gates: OK"
