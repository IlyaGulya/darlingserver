#!/usr/bin/env bash
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$here/contract-test-lib.sh"
contract_test_init standard_signal_coalescing_contract_test

cd "${DSERVER_SRC_ROOT:-$here/..}"

thread_h=internal-include/darlingserver/thread.hpp
thread_cpp=src/thread.cpp

require_grep "_pendingStandardSignalMask" "$thread_h" \
	"Thread does not carry a pending standard-signal mask"

send_signal_body="$(sed -n '/void DarlingServer::Thread::sendSignal/,/^};/p' "$thread_cpp")"
process_signal_body="$(sed -n '/void DarlingServer::Thread::processSignal/,/dtape_thread_process_signal/p' "$thread_cpp")"

require_text "signal != SIGCHLD" "$send_signal_body" \
	"sendSignal does not retain SIGCHLD's exceptional delivery semantics"
require_not_grep "SIGUSR1" <(printf '%s\n' "$send_signal_body") \
	"sendSignal still exempts SIGUSR1 from standard-signal coalescing"
require_text "_markCoalescedStandardSignalPendingLocked" "$send_signal_body" \
	"sendSignal does not call the production standard-signal suppression helper"
require_text "_pendingStandardSignalMask &= ~(1ull << signal)" "$send_signal_body" \
	"sendSignal does not clear pending mask on tgkill/process errors"
require_text "StandardSignalPendingClear" "$process_signal_body" \
	"processSignal does not clear pending standard signal state at completion"
require_text "_pendingStandardSignalMask &= ~(1ull << linuxSignalNumber)" "$process_signal_body" \
	"processSignal clear guard does not reset the delivered signal bit"

echo "STANDARD_SIGNAL_COALESCING_CONTRACT_OK"
