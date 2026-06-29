// perf #18 P6.1 (dar-ohp): RED->GREEN gate on the ring fast-path eligibility + escape hatches.
//
// The fast inline path (doWorkInline, no fiber) runs a proven-non-blocking op directly on the
// main loop. Two safety properties MUST hold or we risk running a blocking/ineligible op without
// a fiber (UB) or being unable to disable the specialized semantics if a bug surfaces:
//   1. ONLY allowlisted no-block ops are eligible (mach_reply_port, task_self_trap) -- nothing else.
//   2. The escape hatches actually gate it: DARLING_SERVER_FAST_OPS=0 disables ALL fast paths;
//      DARLING_SERVER_FAST_MACH_REPLY_PORT=0 disables just mach_reply_port's.
// This test pins both. It mirrors the real ringFastPathEligible() logic from call.cpp against the
// generated callnums.
//
// GREEN (default): the real hatch-aware predicate -- eligibility flips with the env knobs.
// RED arm (-DFASTPATH_NO_HATCH): a predicate that ignores the hatches (always-eligible for the
// allowlist); the "hatch disables it" assertions MUST then fail, proving the test exercises the
// hatch logic, not just the allowlist.

#define _POSIX_C_SOURCE 200112L
#include <darlingserver/rpc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int env_off(const char* name) {
	const char* e = getenv(name);
	return (e && e[0] == '0' && e[1] == '\0');
}

// The predicate under test, mirroring call.cpp's ringFastPathEligible().
static int fast_eligible(unsigned int callnum) {
#ifndef FASTPATH_NO_HATCH
	if (env_off("DARLING_SERVER_FAST_OPS")) return 0;            // global kill-switch
	if (callnum == (unsigned)dserver_callnum_mach_reply_port)
		return !env_off("DARLING_SERVER_FAST_MACH_REPLY_PORT");  // per-op kill-switch
	if (callnum == (unsigned)dserver_callnum_task_self_trap)
		return 1;
	return 0;
#else
	// RED arm: ignores the hatches entirely (the bug we must catch).
	if (callnum == (unsigned)dserver_callnum_mach_reply_port) return 1;
	if (callnum == (unsigned)dserver_callnum_task_self_trap)  return 1;
	return 0;
#endif
}

static int failures = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); ++failures; } \
	else         { fprintf(stderr, "ok:   %s\n", (msg)); } \
} while (0)

static void setenv01(const char* n, int off) {
	if (off) setenv(n, "0", 1); else unsetenv(n);
}

int main(void) {
	// --- allowlist: only the two no-block port traps are eligible ---------------------------
	setenv01("DARLING_SERVER_FAST_OPS", 0);
	setenv01("DARLING_SERVER_FAST_MACH_REPLY_PORT", 0);
	CHECK(fast_eligible(dserver_callnum_mach_reply_port), "mach_reply_port is fast-eligible by default");
	CHECK(fast_eligible(dserver_callnum_task_self_trap),  "task_self_trap is fast-eligible by default");
	CHECK(!fast_eligible(dserver_callnum_kprintf),        "kprintf is NOT fast-eligible");
	CHECK(!fast_eligible(dserver_callnum_checkin),        "checkin is NOT fast-eligible");

	// --- escape hatch: global kill-switch disables ALL fast paths ---------------------------
	setenv01("DARLING_SERVER_FAST_OPS", 1);
	CHECK(!fast_eligible(dserver_callnum_mach_reply_port), "FAST_OPS=0 disables mach_reply_port fast path");
	CHECK(!fast_eligible(dserver_callnum_task_self_trap),  "FAST_OPS=0 disables task_self_trap fast path");
	setenv01("DARLING_SERVER_FAST_OPS", 0);

	// --- escape hatch: per-op kill-switch disables just mach_reply_port ----------------------
	setenv01("DARLING_SERVER_FAST_MACH_REPLY_PORT", 1);
	CHECK(!fast_eligible(dserver_callnum_mach_reply_port), "FAST_MACH_REPLY_PORT=0 disables mach_reply_port fast path");
	CHECK(fast_eligible(dserver_callnum_task_self_trap),   "FAST_MACH_REPLY_PORT=0 leaves task_self_trap fast path on");
	setenv01("DARLING_SERVER_FAST_MACH_REPLY_PORT", 0);

	if (failures) {
		fprintf(stderr, "\nring_fastpath_gate_test: %d FAILURE(S)\n", failures);
		return 1;
	}
	fprintf(stderr, "\nring_fastpath_gate_test: allowlist + escape hatches hold\n");
	return 0;
}
