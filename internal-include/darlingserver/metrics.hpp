/**
 * This file is part of Darling.
 *
 * Copyright (C) 2026 Darling developers
 *
 * Darling is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Darling is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Darling.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _DARLINGSERVER_METRICS_HPP_
#define _DARLINGSERVER_METRICS_HPP_

// perf #0 (dar-dar6x4-perf-5dq.6): in-server progress + timing metrics.
//
// Why this exists: from OUTSIDE the server you cannot reliably tell "the Darling
// workload is slow but progressing" from "it is genuinely wedged" -- log-file size,
// CPU%, and single /proc snapshots all conflate the two (this misdiagnosed dar-l3a as
// a stall when gettext was simply building slowly). The robust signal is a monotonic
// count of work actually done plus the latency of that work, read straight from the
// server over a dedicated UNIX socket.
//
// Design constraints:
//  - hot-path increments must be cheap and lock-free -> std::atomic<uint64_t> with
//    relaxed ordering (these are statistics, not synchronization).
//  - latency is recorded into a fixed log2-bucketed histogram (no per-sample storage,
//    no allocation); quantiles are computed from the buckets at snapshot time.
//  - the snapshot is produced on the main event-loop thread (the stat socket is just
//    another epoll fd), so reading atomics there is naturally consistent enough.

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <string>
#include <array>

namespace DarlingServer {
	// A lock-free latency histogram with log2 buckets.
	// bucket i counts samples whose value (in microseconds) is in [2^(i-1), 2^i)
	// (bucket 0 = [0,1)). With 32 buckets we cover up to ~2^31 us (~35 min), which
	// is far beyond any sane per-RPC latency.
	class LatencyHistogram {
	public:
		static constexpr size_t kBucketCount = 32;

		void record(uint64_t microseconds) {
			size_t idx = bucketFor(microseconds);
			_buckets[idx].fetch_add(1, std::memory_order_relaxed);
			_count.fetch_add(1, std::memory_order_relaxed);
			_sum.fetch_add(microseconds, std::memory_order_relaxed);
			// monotonic running max
			uint64_t prev = _max.load(std::memory_order_relaxed);
			while (microseconds > prev && !_max.compare_exchange_weak(prev, microseconds, std::memory_order_relaxed)) {
				// prev was reloaded by compare_exchange_weak; loop
			}
		}

		uint64_t count() const { return _count.load(std::memory_order_relaxed); }
		uint64_t sum()   const { return _sum.load(std::memory_order_relaxed); }
		uint64_t max()   const { return _max.load(std::memory_order_relaxed); }
		uint64_t mean()  const {
			uint64_t c = count();
			return (c == 0) ? 0 : (sum() / c);
		}

		// Approximate quantile (in microseconds) from the buckets. We return the upper
		// edge of the bucket in which the requested rank falls -- an over-estimate
		// bounded by the bucket width (<=2x), which is fine for "is it 5us or 500ms".
		uint64_t quantile(double q) const {
			uint64_t c = count();
			if (c == 0) {
				return 0;
			}
			uint64_t target = static_cast<uint64_t>(q * static_cast<double>(c));
			if (target >= c) {
				target = c - 1;
			}
			uint64_t cumulative = 0;
			for (size_t i = 0; i < kBucketCount; ++i) {
				cumulative += _buckets[i].load(std::memory_order_relaxed);
				if (cumulative > target) {
					return bucketUpperEdge(i);
				}
			}
			return bucketUpperEdge(kBucketCount - 1);
		}

	private:
		static size_t bucketFor(uint64_t microseconds) {
			if (microseconds == 0) {
				return 0;
			}
			// index = floor(log2(us)) + 1, clamped to the last bucket
			size_t idx = 0;
			uint64_t v = microseconds;
			while (v > 0) {
				++idx;
				v >>= 1;
			}
			// now idx == floor(log2(us)) + 1
			return (idx >= kBucketCount) ? (kBucketCount - 1) : idx;
		}

		static uint64_t bucketUpperEdge(size_t i) {
			if (i == 0) {
				return 1;
			}
			if (i >= kBucketCount - 1) {
				return UINT64_C(1) << (kBucketCount - 1);
			}
			return UINT64_C(1) << i;
		}

		std::array<std::atomic<uint64_t>, kBucketCount> _buckets {};
		std::atomic<uint64_t> _count {0};
		std::atomic<uint64_t> _sum {0};
		std::atomic<uint64_t> _max {0};
	};

	class Metrics {
	public:
		static Metrics& shared();

		// ---- monotonic counters (hot path) ----
		std::atomic<uint64_t> rpcsServiced {0};   // RPC calls fully processed
		std::atomic<uint64_t> repliesSent {0};    // reply datagrams sent
		std::atomic<uint64_t> checkins {0};       // checkin RPCs (process/thread registration)
		std::atomic<uint64_t> forks {0};          // fork-checkins specifically
		std::atomic<uint64_t> messagesReceived {0};
		// perf #2b (dar-dar6x4-perf-5dq.8): inline fast-path accounting. A call run to
		// completion directly on the main event loop (no worker wakeup) increments
		// inlineHandled; a call that suspended its microthread (so it must finish on the
		// worker pool when resumed) increments queuedToPool. Under a healthy fast-path the
		// vast majority of cheap RPCs (checkin etc.) should be inlineHandled.
		std::atomic<uint64_t> inlineHandled {0};
		std::atomic<uint64_t> queuedToPool {0};

#ifdef DSERVER_RING_TRANSPORT
		// perf #18 P4 (dar-dar6x4-perf-5dq.33): wake-model accounting. ringServicedSpin =
		// requests drained by the main-loop spin phase (the HOT path: guest skipped the
		// doorbell, server found the request by polling). ringServicedDoorbell = requests
		// drained by the eventfd Monitor callback (the COLD path: guest doorbelled because the
		// server was sleeping/armed). ringWakesIssued/ringWakesSkipped = FUTEX_WAKE on the
		// reply path actually done vs elided because no guest was parked. A healthy hot stream
		// is mostly ringServicedSpin + ringWakesSkipped; both prove zero per-call syscalls.
		std::atomic<uint64_t> ringServicedSpin {0};
		std::atomic<uint64_t> ringServicedDoorbell {0};
		std::atomic<uint64_t> ringWakesIssued {0};
		std::atomic<uint64_t> ringWakesSkipped {0};
		std::atomic<uint64_t> ringDoorbellsReceived {0}; // eventfd wakes the server actually drained

		// perf #18 P6.1 step 2 (dar-ohp): surgical direct-dispatch fast path for mach_reply_port.
		// ringFastHit = a mach_reply_port request serviced WITHOUT building a Call/Message or
		// re-looking-up the thread in the registry (doMachReplyPortInline -> dtape trap -> ring
		// publish). ringFastFallback = a request that matched the callnum but failed the shape
		// guard or whose inline dispatch declined -> took the generic step-1 callFromMessage path.
		std::atomic<uint64_t> ringFastHit {0};
		std::atomic<uint64_t> ringFastFallback {0};
		// perf #18 P7 (dar-my8): the rest of the gist-required per-op fast-path accounting.
		// ringFastFail  = an inline fast op that could not complete cleanly (declined late / publish
		//                 lost) -- distinct from a fallback (which is a deliberate route to generic).
		// ringS2cFull   = publishReply() found the s2c ring full (reply could not be published there;
		//                 the caller UDS-falls-back). A nonzero value under load = guest not draining.
		// ringFastSuspend = an op the fast path ran as non-blocking actually SUSPENDED -- a contract
		//                 violation (allowlist misclassification). MUST stay 0; nonzero = a bug.
		std::atomic<uint64_t> ringFastFail {0};
		std::atomic<uint64_t> ringS2cFull {0};
		std::atomic<uint64_t> ringFastSuspend {0};
		// perf #18 P8 D3 (dar-1il.3.1.1): the duplex lane. ringDuplexS2c = an S2C upcall delivered to
		// a ring-parked caller via the duplex mailbox (instead of the UDS S2C path) AND completed (the
		// correlated reply came back + the server resumed). ringDuplexReject = a duplex reply that
		// failed correlation (wrong parent/upcall id) -- the server refused to resume on it and fell
		// the op back to UDS. Both MUST be 0 unless the duplex selftest is actively driven; a nonzero
		// ringDuplexS2c on a normal boot would mean a real op wrongly took the duplex path (a bug --
		// the conjunction guard is supposed to gate it to the env-driven selftest only).
		std::atomic<uint64_t> ringDuplexS2c {0};
		std::atomic<uint64_t> ringDuplexReject {0};
		// perf #18 P8 D4 (dar-1il.3.2.1): duplex PARENT routing counters (distinct from the S2C-upcall
		// counters above). ringDuplexParent = a real op (mach_port_deallocate) was DISPATCHED onto the
		// duplex lane (the caller advertised the cap, the shape was accepted, it ran on the fiber). This
		// is the proof a deallocate actually RODE the duplex lane -- it bumps even when the op needs NO
		// S2C (the common refcount>1 case), unlike ringDuplexS2c which only counts an actual munmap S2C.
		// ringDuplexDecline = a duplex-routed op was DECLINED pre-dispatch (no cap / bad shape) and a
		// DSERVER_RING_DUPLEX_DECLINE reply was sent so the guest UDS-falls-back (no mutation, no
		// double-effect). NOTE: in Darling, mach_port_deallocate does not drive a munmap S2C in practice
		// (mach_make_memory_entry_64 is stubbed), so ringDuplexParent can be >0 while ringDuplexS2c stays
		// 0 -- the deallocate rode the lane but never needed the caller-S2C the lane exists to service.
		std::atomic<uint64_t> ringDuplexParent {0};
		std::atomic<uint64_t> ringDuplexDecline {0};
		// perf #18 P8 D5/D6: the vm_deallocate-via-duplex proof harness
		// counters (option 1, gist 3e928115). The proof is armed by a MARKER FILE (never inherited env) and
		// AUTO-DISARMS after the first successful caller-S2C. A GREEN proof is:
		//   s2cMunmapToCaller>=1 AND ringDuplexVmdeallocParent>=1 AND _s2c>=1 AND _final>=1 AND _timeout==0
		//   AND boot reaches shellspawn.
		// vmdeallocParent  = a launchd vm_deallocate was DISPATCHED onto the duplex lane (rode the lane).
		// vmdeallocDecline = a vm_deallocate routing was DECLINED pre-dispatch (not armed / bad shape / budget 0).
		// vmdeallocS2c     = a real UPCALL_MUNMAP for a vm_deallocate parent completed over the duplex mailbox.
		// vmdeallocFinal   = the parent vm_deallocate op's final ring reply was published (op finished on lane).
		// vmdeallocTimeout = a duplex munmap upcall reply was NOT harvested in the bound -> failed closed.
		// vmdeallocDisarmed= the proof auto-disarmed (budget hit 0 after a success) -> no further routing.
		std::atomic<uint64_t> ringDuplexVmdeallocParent {0};
		std::atomic<uint64_t> ringDuplexVmdeallocDecline {0};
		std::atomic<uint64_t> ringDuplexVmdeallocS2c {0};
		std::atomic<uint64_t> ringDuplexVmdeallocFinal {0};
		std::atomic<uint64_t> ringDuplexVmdeallocTimeout {0};
		std::atomic<uint64_t> ringDuplexVmdeallocDisarmed {0};
		// perf #18 P8 D6 (caller-S2C sideband) ATTRIBUTION: for every caller-S2C munmap upcall (_s2cPerform
		// munmap to the CURRENT thread), classify the active PARENT op's lane. munmapRingParent = the caller
		// has an active ring-originated parent (_ringReplyPending) -> the direct cure target for the duplex
		// sideband. munmapUdsParent = an active UDS-originated call (today serviced via recvmsg = NOT a
		// ring-deadlock, but a FUTURE hazard if that op is ring-migrated). munmapNoParent = no active managed
		// call (pure server-internal teardown). The measured Darling boot: ALL caller-S2C munmaps go under a
		// UDS parent (mach_msg_overwrite OOL teardown) -> the hazard is reachable but UDS-only today.
		std::atomic<uint64_t> s2cMunmapRingParent {0};
		std::atomic<uint64_t> s2cMunmapUdsParent {0};
		std::atomic<uint64_t> s2cMunmapNoParent {0};

#ifdef DSERVER_RING_PHASE_PROF
		// perf #18 P6 (dar-aw2): cycle-decompose the hot ring RPC. rdtsc brackets in
		// ringServiceThread/publishReply accumulate per-phase TSC cycles + a sample count, so we
		// can read mean cycles/phase from the stat socket. Pure diagnostic (default OFF, this whole
		// block compiles out); the brackets read the TSC, which is far below a phase's own cost so
		// they don't perturb the breakdown the way clock_gettime would.
		//   drain    = consumer_begin + copy the slot transport header out
		//   dispatch = callFromMessage (rebuild Message, registry lookup, decode)
		//   body     = doWork() -- the inline Mach operation itself
		//   publish  = publishReply (claim s2c slot, write reply, release-store)
		std::atomic<uint64_t> phaseDrainCycles {0};
		std::atomic<uint64_t> phaseDispatchCycles {0};
		std::atomic<uint64_t> phaseBodyCycles {0};
		std::atomic<uint64_t> phasePublishCycles {0};
		std::atomic<uint64_t> phaseSamples {0};
#endif
#endif

		// ---- gauges sampled at snapshot time (set by the owner) ----
		// These are filled in by Server when producing a snapshot, not on the hot path.

		// ---- latency histograms ----
		LatencyHistogram rpcLatency;       // service time of any RPC (worker entry -> reply)
		LatencyHistogram checkinLatency;   // service time of checkin RPCs
		LatencyHistogram forkLatency;      // service time of fork-checkin RPCs (incl. parent coordination)

		// ---- per-call-number breakdown (perf #9 / dar-dar6x4-perf-5dq.16) ----
		// The real-build profile (perf #6) put ~57% of guest wall-clock in recvmsg
		// waiting for an RPC reply. To aim the recv-spin tuning (perf #7) at the calls
		// that actually dominate, we record, per call number, how many times it was
		// serviced and the distribution of its server-side service time. Frequency is
		// the key lever: every call pays one full round-trip wakeup that the spin can
		// elide, so the most NUMEROUS calls are the ones worth spinning for.
		//
		// Call numbers are sequential 1..~80 (the high UNMANAGED bit is masked off when
		// indexing), so a fixed lock-free array indexed by the low bits is both cheap
		// and allocation-free on the hot path. 256 covers the whole range with margin.
		static constexpr size_t kMaxCallNumbers = 256;
		std::array<std::atomic<uint64_t>, kMaxCallNumbers> perCallCount {};
		std::array<LatencyHistogram, kMaxCallNumbers> perCallLatency {};

		// Record one serviced RPC of the given call number with the given service time.
		// callNumber is the raw dserver_callnum value (the UNMANAGED flag, if set, is
		// masked off for indexing). Safe to call from any worker; lock-free.
		void recordCall(uint32_t callNumber, uint64_t microseconds) {
			size_t idx = callNumber & 0xffu; // low byte: call numbers are < 256
			if (idx >= kMaxCallNumbers) {
				return;
			}
			perCallCount[idx].fetch_add(1, std::memory_order_relaxed);
			perCallLatency[idx].record(microseconds);
		}

		// last reply timestamp (CLOCK_MONOTONIC microseconds), for last_reply_age_ms
		std::atomic<uint64_t> lastReplyMonoUs {0};

		// process start timestamp (CLOCK_MONOTONIC microseconds)
		uint64_t startMonoUs {0};

		// Current monotonic time in microseconds.
		static uint64_t nowMonoUs();

#ifdef DSERVER_RING_PHASE_PROF
		// perf #18 P6: raw TSC read for sub-microsecond phase brackets. x86-only diagnostic.
		static inline uint64_t rdtscCycles() {
#if defined(__x86_64__) || defined(__i386__)
			unsigned hi, lo;
			__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
			return ((uint64_t)hi << 32) | lo;
#else
			return nowMonoUs() * 1000; // fallback (coarse)
#endif
		}
#endif

		// Build a JSON snapshot string. `extra` lets the caller inject gauges it owns
		// (workqueue_depth, workers_busy/total, clients_blocked_in_rpc) as already-
		// formatted "\"key\": value" fragments, comma-separated, no surrounding braces.
		std::string snapshotJSON(const std::string& extraGauges) const;

	private:
		Metrics() = default;
	};
};

#endif // _DARLINGSERVER_METRICS_HPP_
