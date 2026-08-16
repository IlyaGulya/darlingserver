#pragma once

#include <sched.h>
#include <cstdlib>
#include <darling_lifecycle_cohort.h>

class LifecycleCohortOwner {
public:
	LifecycleCohortOwner() = default;
	~LifecycleCohortOwner() {
		for (unsigned int attempt = 0; _controller && attempt < 3; ++attempt)
			(void)finish();
		/* Never return from destruction while an owning pointer would be lost. */
		if (_controller)
			std::abort();
	}
	LifecycleCohortOwner(const LifecycleCohortOwner&) = delete;
	LifecycleCohortOwner& operator=(const LifecycleCohortOwner&) = delete;

	bool adopt(struct darling_lifecycle_cohort_controller* controller) {
		if (_phase != Phase::Empty || _controller || !controller)
			return false;
		_controller = controller;
		_phase = Phase::Active;
		return true;
	}

	explicit operator bool() const { return _phase != Phase::Empty && _controller != nullptr; }
	struct darling_lifecycle_cohort_controller* get() const { return _controller; }
	struct darling_lifecycle_cohort_controller* takeRecoveryForHandoff() {
		if (_phase != Phase::RecoveryPending || !_controller)
			return nullptr;
		auto* controller = _controller;
		_controller = nullptr;
		_phase = Phase::Empty;
		return controller;
	}

	int finish() {
		if (!_controller)
			return DARLING_LIFECYCLE_FINISH_OK;
		if (_phase == Phase::Cleaning) {
			const int result = darling_lifecycle_cohort_finish(_controller);
			if (result == DARLING_LIFECYCLE_FINISH_CLEANUP_PENDING)
				return result;
			_controller = nullptr;
			_phase = Phase::Empty;
			return result;
		}
		if (_phase == Phase::RecoveryPending)
			return DARLING_LIFECYCLE_FINISH_RECOVERY_PENDING;
		if (_phase == Phase::Active) {
			for (unsigned int attempt = 0; attempt < 3; ++attempt) {
				const int result = darling_lifecycle_cohort_finish(_controller);
				if (result == DARLING_LIFECYCLE_FINISH_CLEANUP_PENDING) {
					_phase = Phase::Cleaning;
					return result;
				}
				if (result == DARLING_LIFECYCLE_FINISH_RECOVERY_PENDING) {
					_phase = Phase::RecoveryPending;
					return result;
				}
				if (result != DARLING_LIFECYCLE_FINISH_DRAIN_PENDING) {
					_controller = nullptr;
					_phase = Phase::Empty;
					return result;
				}
				sched_yield();
			}
			_phase = Phase::Abandoning;
		}
		/* Abandoning is monotonic: normal finish is never called again. */
		const int abandoned = darling_lifecycle_cohort_abandon(_controller);
		if (abandoned == DARLING_LIFECYCLE_ABANDON_PENDING)
			return DARLING_LIFECYCLE_ABANDON_PENDING;
		_controller = nullptr;
		_phase = Phase::Empty;
		return abandoned == 0 ? DARLING_LIFECYCLE_FINISH_ABANDONED :
			DARLING_LIFECYCLE_FINISH_ERROR;
	}

private:
	enum class Phase { Empty, Active, Cleaning, RecoveryPending, Abandoning };
	struct darling_lifecycle_cohort_controller* _controller = nullptr;
	Phase _phase = Phase::Empty;
};
