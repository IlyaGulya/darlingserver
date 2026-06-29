// perf #18 (dar-dar6x4-perf-5dq.30) P3 RED->GREEN gate for the server-side C2S ring ELIGIBILITY
// ALLOWLIST (call.cpp's darRingServiceC2S). Unlike ring_datapath_test.cpp (hermetic, callnum-
// agnostic), this gate pins the EXACT set of real dserver_callnum_* values that may ride the
// ring -- the actual new code added when mach_reply_port was migrated.
//
// It must use the GENERATED rpc.h (build-dir only) for the real enum values, so it is compiled
// with the build include dir injected by run-ring-shm-validate.sh (DSERVER_GEN_INC).
//
//   GREEN arm: the current allowlist predicate (task_self_trap OR mach_reply_port). Asserts both
//              migrated ops are eligible and a representative non-migrated op (kprintf) is NOT.
//   RED   arm (-DALLOWLIST_OLD): the pre-migration predicate (task_self_trap only). The
//              "mach_reply_port is eligible" assertion MUST then fail -> proves the gate is
//              actually exercising the allowlist change, not a tautology.
//
// SECURITY note: the allowlist is the trust boundary that keeps an untrusted guest from driving
// arbitrary server calls over the ring. Only no-arg, single-uint32-port-reply traps belong here;
// anything with a request body or pointers must stay on the audited UDS path for now.

#include <darlingserver/rpc.h>

#include <cstdint>
#include <cstdio>

// Mirror of the predicate in src/call.cpp:darRingServiceC2S(). Keep these in lockstep: if you
// add a callnum to the allowlist there, add it here (and a GREEN assertion below).
static bool ringEligible(uint32_t callnum) {
#ifdef ALLOWLIST_OLD
	// pre-migration: task_self_trap only
	return (callnum == dserver_callnum_task_self_trap);
#else
	return (callnum == dserver_callnum_task_self_trap) ||
	       (callnum == dserver_callnum_mach_reply_port);
#endif
}

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); ++failures; } } while (0)

int main(void) {
	// Both migrated ops must be eligible.
	CHECK(ringEligible(dserver_callnum_task_self_trap), "task_self_trap is ring-eligible");
	CHECK(ringEligible(dserver_callnum_mach_reply_port), "mach_reply_port is ring-eligible");

	// A representative op that is NOT migrated must be rejected (stays on UDS). kprintf takes a
	// request body, so it must NEVER be ring-eligible under the no-arg-only P3 rule.
	CHECK(!ringEligible(dserver_callnum_kprintf), "kprintf is NOT ring-eligible (has a body)");
	// thread_self/host_self are caller-deallocated self ports -- not migrated in P3.
	CHECK(!ringEligible(dserver_callnum_host_self_trap), "host_self_trap is NOT ring-eligible (P3)");

	if (failures == 0) {
		printf("ring allowlist gate: all checks passed (%s)\n",
#ifdef ALLOWLIST_OLD
		       "OLD"
#else
		       "CURRENT"
#endif
		);
		return 0;
	}
	fprintf(stderr, "ring allowlist gate: %d FAILURES\n", failures);
	return 1;
}
