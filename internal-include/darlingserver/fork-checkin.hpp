#ifndef _DARLINGSERVER_FORK_CHECKIN_HPP_
#define _DARLINGSERVER_FORK_CHECKIN_HPP_

#include <darlingserver/duct-tape.h>

namespace DarlingServer {
	enum class ForkCheckinWaitResult {
		Observed,
		Interrupted,
		TimedOut,
		Error,
	};

	constexpr unsigned int ForkCheckinWaitTimeoutSeconds = 30;

	inline ForkCheckinWaitResult waitForForkCheckin(dtape_semaphore_t* semaphore, unsigned int timeoutSeconds = ForkCheckinWaitTimeoutSeconds) {
		switch (dtape_semaphore_down_timeout(semaphore, timeoutSeconds)) {
			case dtape_semaphore_wait_result_ok:
				return ForkCheckinWaitResult::Observed;
			case dtape_semaphore_wait_result_interrupted:
				return ForkCheckinWaitResult::Interrupted;
			case dtape_semaphore_wait_result_timed_out:
				return ForkCheckinWaitResult::TimedOut;
			default:
				return ForkCheckinWaitResult::Error;
		}
	}
}

#endif
