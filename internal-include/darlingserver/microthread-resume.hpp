#ifndef _DARLINGSERVER_MICROTHREAD_RESUME_HPP_
#define _DARLINGSERVER_MICROTHREAD_RESUME_HPP_

namespace DarlingServer {
	inline bool microthreadRecordResume(bool running, bool suspended, bool& resumePermit) {
		if (!running && !suspended) {
			return false;
		}
		if (resumePermit) {
			return false;
		}
		resumePermit = true;
		return suspended && !running;
	}

	inline bool microthreadConsumePendingResume(bool& resumePermit) {
		if (!resumePermit) {
			return false;
		}
		resumePermit = false;
		return true;
	}

	inline bool microthreadConsumeResumeDuringSuspend(bool& suspended, bool& resumePermit) {
		if (!microthreadConsumePendingResume(resumePermit)) {
			return false;
		}
		suspended = false;
		return true;
	}

	inline bool microthreadShouldScheduleAfterStop(bool resumePermit, bool suspended, bool terminating, bool dead) {
		return resumePermit && suspended && !terminating && !dead;
	}
}

#endif
