#!/usr/bin/env bash
# Static gate for UDS RPC correlation: generated clients must tag each request with a packed
# serial in the existing callnum word, generated/server replies must echo it, and guest receive
# validation must match base-callnum+serial without changing the wire header size.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
GEN="$ROOT/scripts/generate-rpc-wrappers.py"
CALL="$ROOT/src/call.cpp"
THREAD="$ROOT/src/thread.cpp"
XNU_RPC="${XNU_RPC_DEFS:-$ROOT/../xnu/darling/src/libsystem_kernel/emulation/src/linux_premigration/resources/dserver-rpc-defs.c}"

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

! grep -q "uint64_t serial" "$GEN" || fail "generated RPC headers grew a serial field"
grep -q "DSERVER_CALL_SERIAL_FLAG" "$GEN" || fail "generated RPC header has no packed serial flag"
grep -q "dserver_rpc_callnum_base" "$GEN" || fail "generated RPC header has no callnum base helper"
grep -q "dserver_rpc_callnum_serial" "$GEN" || fail "generated RPC header has no callnum serial helper"
grep -q "dserver_rpc_callnum_with_serial" "$GEN" || fail "generated RPC header has no serial packing helper"
grep -q "__dserver_rpc_next_serial" "$GEN" || fail "generated guest wrappers have no serial allocator"
grep -q "dserver_rpc_callnum_with_serial(dserver_callnum_" "$GEN" || fail "generated calls do not pack a serial"
grep -q "replyStruct->header.number = _header.number" "$GEN" || fail "generated replies do not echo packed request number"
grep -q "dserver_rpc_callnum_serial(reply_msg.reply.header.number) != dserver_rpc_callnum_serial(call.header.number)" "$GEN" \
	|| fail "generated receive path still matches replies by base call number only"

grep -q "replyStruct->number = header->number" "$CALL" || fail "manual error replies do not echo packed request number"
grep -q "dserver_rpc_callnum_base(header->number)" "$CALL" || fail "server dispatch does not mask packed serial bits"
! grep -q "hdr->serial = seq" "$CALL" || fail "ring-dispatch synthetic RPC headers still write removed serial field"
! grep -q "reply.header.serial" "$THREAD" || fail "manual mach_reply_port fallback still writes removed serial field"

[ -f "$XNU_RPC" ] || fail "xnu dserver-rpc-defs.c not found at $XNU_RPC"
! grep -q "__dserver_rpc_push_reply_next_serial" "$XNU_RPC" || fail "push_reply incorrectly allocates serial for special callnum"
! grep -q "\\.serial =" "$XNU_RPC" || fail "push_reply still writes removed serial field"

echo "rpc_serial_contract_test: UDS RPC replies match base-callnum+packed-serial"
