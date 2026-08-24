#pragma once

#include <chrono>
#include <sys/types.h>

namespace DarlingServer {

// Block lifecycle termination signals before publishing any externally
// discoverable session identity.  The server event loop consumes them later
// through signalfd; keeping this in the production helper makes the ordering
// directly testable.
int blockRootlessLifecycleTerminationSignals();

// Drain every child owned by the rootless Darlingserver subreaper.  Signals
// are delivered only through pidfds opened from the kernel's direct-child
// census; the function does not fall back to raw PIDs.
int drainRootlessSessionChildren(
	pid_t retainedControllerPID,
	std::chrono::milliseconds gracefulTimeout,
	std::chrono::milliseconds killTimeout
);

}
