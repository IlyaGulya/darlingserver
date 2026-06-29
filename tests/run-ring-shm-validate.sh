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
