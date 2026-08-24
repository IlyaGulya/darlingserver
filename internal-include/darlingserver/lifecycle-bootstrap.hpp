#pragma once

#include <darling_lifecycle_cohort.h>

namespace DarlingServer {

struct ReceivedLifecycleBootstrap {
	int directoryFD = -1;
	darling_lifecycle_cohort_bootstrap envelope = {};
};

bool sendLifecycleBootstrap(
	int socket,
	int directoryFD,
	const darling_lifecycle_cohort_bootstrap& envelope
);

ReceivedLifecycleBootstrap receiveLifecycleBootstrap(int socket);

bool installLifecycleEnvelope(
	const darling_lifecycle_cohort_bootstrap& envelope
);

}
