#!/usr/bin/env bash
# Static gate for reachable duct-tape stubs: guest-controlled or common-fault paths must fail
# closed with an error/default, not abort/panic the whole darlingserver process.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
DTAPE="${DTAPE_SRC:-$HERE/../duct-tape/src}"

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

[ -d "$DTAPE" ] || fail "duct-tape src dir not found at $DTAPE"

task_c="$DTAPE/task.c"
thread_c="$DTAPE/thread.c"
stubs_c="$DTAPE/stubs.c"

grep -q "proc_get_effective_task_policy" "$task_c" || fail "policy stub not found"
if sed -n '/int proc_get_effective_task_policy/,/};/p' "$task_c" | grep -Eq 'panic\(|dtape_stub_unsafe'; then
	fail "proc_get_effective_task_policy still aborts/panics on reachable flavor input"
fi
sed -n '/int proc_get_effective_task_policy/,/};/p' "$task_c" | grep -q "TASK_POLICY_DARWIN_BG" \
	|| fail "proc_get_effective_task_policy does not cover non-role policy flavors"

if sed -n '/kern_return_t handle_ux_exception/,/};/p' "$thread_c" | grep -Eq 'dtape_stub_unsafe|abort\(|panic\('; then
	fail "handle_ux_exception still aborts on common exception fallback"
fi
sed -n '/kern_return_t handle_ux_exception/,/};/p' "$thread_c" | grep -q "thread_set_pending_signal" \
	|| fail "handle_ux_exception no longer queues the translated signal"

if sed -n '/kern_return_t thread_abort_safely/,/};/p' "$thread_c" | grep -q "return KERN_SUCCESS"; then
	fail "thread_abort_safely still fabricates success"
fi
sed -n '/kern_return_t thread_abort_safely/,/};/p' "$thread_c" | grep -q "return KERN_FAILURE" \
	|| fail "thread_abort_safely does not fail closed"

if sed -n '/boolean_t IOTaskHasEntitlement/,/};/p' "$stubs_c" | grep -q "return TRUE"; then
	fail "IOTaskHasEntitlement still grants entitlements by default"
fi
sed -n '/boolean_t IOTaskHasEntitlement/,/};/p' "$stubs_c" | grep -q "return FALSE" \
	|| fail "IOTaskHasEntitlement does not deny by default"

echo "duct_tape_fail_closed_stubs_test: reachable stubs fail closed"
