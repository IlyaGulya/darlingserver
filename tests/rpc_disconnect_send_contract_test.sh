#!/usr/bin/env bash
set -euo pipefail

cd "${DSERVER_SRC_ROOT:-$(dirname "$0")/..}"

gen=scripts/generate-rpc-wrappers.py
dbg=tools/dserverdbg-rpc-defs.h

fail() {
	echo "rpc_disconnect_send_contract_test: $*" >&2
	exit 1
}

grep -q "QUIET_SEND_DISCONNECT" "$gen" \
	|| fail "generator has no QUIET_SEND_DISCONNECT flag"
grep -q "dserver_rpc_hooks_is_disconnect_status" "$gen" \
	|| fail "generator does not call the disconnect-status hook"
grep -q "('interrupt_enter'.*PUSH_UNKNOWN_REPLIES | QUIET_SEND_DISCONNECT" "$gen" \
	|| fail "interrupt_enter is not marked quiet-disconnect"
grep -A5 "('pthread_kill'" "$gen" | grep -q "QUIET_SEND_DISCONNECT" \
	|| fail "pthread_kill is not marked quiet-disconnect"
grep -A5 "('pthread_canceled'" "$gen" | grep -q "QUIET_SEND_DISCONNECT" \
	|| fail "pthread_canceled is not marked quiet-disconnect"

grep -q "return dserver_rpc_hooks_get_interrupt_status();" "$gen" \
	|| fail "interruptible send disconnects do not map to interrupt status"
grep -q "return (int)long_status;" "$gen" \
	|| fail "quiet non-interruptible disconnects do not return raw transport status"

disconnect_branch_line="$(grep -n "dserver_rpc_hooks_is_disconnect_status(long_status)" "$gen" | head -1 | cut -d: -f1)"
bad_send_line="$(grep -n "BAD SEND STATUS" "$gen" | head -1 | cut -d: -f1)"
if [ -z "$disconnect_branch_line" ] || [ -z "$bad_send_line" ] || [ "$disconnect_branch_line" -ge "$bad_send_line" ]; then
	fail "disconnect classification must happen before BAD SEND logging"
fi

grep -q "status == -EPIPE" "$dbg" \
	|| fail "debug RPC hooks do not classify EPIPE disconnects"
grep -q "status == -ECONNRESET" "$dbg" \
	|| fail "debug RPC hooks do not classify ECONNRESET disconnects"
grep -q "status == -ENOTCONN" "$dbg" \
	|| fail "debug RPC hooks do not classify ENOTCONN disconnects"
grep -q "status == -ECONNREFUSED" "$dbg" \
	|| fail "debug RPC hooks do not classify ECONNREFUSED disconnects"

echo "RPC_DISCONNECT_SEND_CONTRACT_OK"
