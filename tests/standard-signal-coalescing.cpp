// Deterministic production-helper regression for standard-signal duplicate
// suppression.  This does not model guest signal delivery: it drives the exact
// helper called by Thread::sendSignal() before tgkill.
#include <darlingserver/thread.hpp>

#include <csignal>
#include <cstdint>
#include <cstdio>

namespace DarlingServer {
	struct ThreadWakeTestAdapter {
		static bool markCoalescedStandardSignal(uint64_t& pendingMask, int signal) {
			return Thread::_markCoalescedStandardSignalPendingLocked(pendingMask, signal);
		}
	};
}

int main() {
	uint64_t pendingMask = 0;
	const uint64_t bit = 1ull << SIGUSR1;

	if (!DarlingServer::ThreadWakeTestAdapter::markCoalescedStandardSignal(pendingMask, SIGUSR1)) {
		std::fprintf(stderr, "first SIGUSR1 delivery was incorrectly suppressed\n");
		return 1;
	}
	if (pendingMask != bit) {
		std::fprintf(stderr, "first SIGUSR1 delivery did not set its pending bit\n");
		return 1;
	}
	if (DarlingServer::ThreadWakeTestAdapter::markCoalescedStandardSignal(pendingMask, SIGUSR1)) {
		std::fprintf(stderr, "duplicate pending SIGUSR1 was not suppressed\n");
		return 1;
	}
	if (pendingMask != bit) {
		std::fprintf(stderr, "duplicate SIGUSR1 changed the pending mask\n");
		return 1;
	}

	// processSignal's existing RAII completion clear makes the next delivery
	// eligible; this models that exact mask transition without a timing race.
	pendingMask &= ~bit;
	if (!DarlingServer::ThreadWakeTestAdapter::markCoalescedStandardSignal(pendingMask, SIGUSR1) || pendingMask != bit) {
		std::fprintf(stderr, "SIGUSR1 did not re-arm after completion clear\n");
		return 1;
	}

	return 0;
}
