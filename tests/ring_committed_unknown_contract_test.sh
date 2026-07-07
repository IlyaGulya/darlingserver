#!/usr/bin/env bash
# Static gate for the ring RPC retry contract:
#   * before publish: negative helper result may fall back to UDS
#   * after publish: timeout/bad-reply is committed-unknown and must NOT UDS-retry
#
# The guest helper is intentionally libc-free and mostly static functions, so this test pins the
# source-level contract that prevents future edits from reintroducing post-publish UDS retry.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${XNU_RING_SRC:-$HERE/../../xnu/darling/src/libsystem_kernel/emulation/src/linux_premigration/resources/dserver-ring.c}"
HDR="${XNU_RING_HDR:-$HERE/../../xnu/darling/src/libsystem_kernel/emulation/include/linux_premigration/resources/dserver-ring.h}"

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

[ -f "$SRC" ] || fail "dserver-ring.c not found at $SRC"
[ -f "$HDR" ] || fail "dserver-ring.h not found at $HDR"

grep -q "GR_WAIT_COMMITTED_UNKNOWN" "$SRC" \
	|| fail "guest wait path has no typed committed-unknown state"

grep -q "published: do NOT retry over UDS" "$SRC" \
	|| fail "post-publish helpers do not document/enforce no-UDS-retry"

grep -q "out_code) \\*out_code = KERN_FAILURE" "$SRC" \
	|| fail "post-publish body helpers do not surface KERN_FAILURE without retry"

grep -q "timeout/bad-reply.*never UDS-falls-back" "$HDR" \
	|| fail "public ring helper comments do not state the committed-unknown no-retry contract"

if grep -Eq "gave up[^\\n]*UDS fallback|gave up[^\\n]*UDS-fall-back|timeout[^\\n]*UDS fallback|timeout[^\\n]*UDS-fall-back" "$SRC"; then
	fail "source still describes post-publish timeout as UDS fallback"
fi

if grep -Eq "KERN_SUCCESS[^\\n]*do NOT retry|out_code\\) \\*out_code = 0;[^\\n]*do NOT retry" "$SRC"; then
	fail "committed-unknown path still fabricates success instead of KERN_FAILURE"
fi

echo "ring_committed_unknown_contract_test: post-publish ring misses do not UDS-retry"
