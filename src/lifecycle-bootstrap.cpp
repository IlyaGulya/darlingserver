#include <darlingserver/lifecycle-bootstrap.hpp>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace DarlingServer {

namespace {

constexpr char bootstrapMagic[8] = {'D', 'L', 'C', 'H', 'I', 'L', 'D', '1'};

struct BootstrapPayload {
	char magic[8];
	uint32_t abiVersion;
	darling_lifecycle_cohort_bootstrap envelope;
};

bool validEnvelope(const darling_lifecycle_cohort_bootstrap& envelope) {
	if (envelope.control_name_len == 0 ||
		envelope.control_name_len >= DARLING_LIFECYCLE_CONTROL_NAME_CAPACITY)
		return false;
	for (size_t i = 0; i < envelope.control_name_len; ++i) {
		if (envelope.control_name[i] == 0)
			return false;
	}
	for (size_t i = 0; i < DARLING_LIFECYCLE_NONCE_HEX_BYTES; ++i) {
		const uint8_t value = envelope.nonce_hex[i];
		if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f')))
			return false;
	}
	return true;
}

}

bool sendLifecycleBootstrap(
	int socket,
	int directoryFD,
	const darling_lifecycle_cohort_bootstrap& envelope
) {
	if (directoryFD < 0 || !validEnvelope(envelope)) {
		errno = EINVAL;
		return false;
	}
	BootstrapPayload payload = {};
	memcpy(payload.magic, bootstrapMagic, sizeof(payload.magic));
	payload.abiVersion = DARLING_LIFECYCLE_COHORT_ABI_VERSION;
	payload.envelope = envelope;
	struct iovec iov = {&payload, sizeof(payload)};
	char control[CMSG_SPACE(sizeof(int))] = {};
	struct msghdr message = {};
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof(control);
	struct cmsghdr* header = CMSG_FIRSTHDR(&message);
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = SCM_RIGHTS;
	header->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(header), &directoryFD, sizeof(directoryFD));
	ssize_t result;
	do {
		result = sendmsg(socket, &message, MSG_NOSIGNAL);
	} while (result < 0 && errno == EINTR);
	return result == static_cast<ssize_t>(sizeof(payload));
}

ReceivedLifecycleBootstrap receiveLifecycleBootstrap(int socket) {
	ReceivedLifecycleBootstrap output;
	BootstrapPayload payload = {};
	struct iovec iov = {&payload, sizeof(payload)};
	char control[CMSG_SPACE(sizeof(int))] = {};
	struct msghdr message = {};
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof(control);
	ssize_t result;
	do {
		result = recvmsg(socket, &message, MSG_CMSG_CLOEXEC);
	} while (result < 0 && errno == EINTR);
	struct cmsghdr* header = CMSG_FIRSTHDR(&message);
	if (header && header->cmsg_level == SOL_SOCKET &&
		header->cmsg_type == SCM_RIGHTS && header->cmsg_len >= CMSG_LEN(sizeof(int)))
		memcpy(&output.directoryFD, CMSG_DATA(header), sizeof(output.directoryFD));
	const bool valid = result == static_cast<ssize_t>(sizeof(payload)) &&
		!(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) && header &&
		!CMSG_NXTHDR(&message, header) &&
		header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS &&
		header->cmsg_len == CMSG_LEN(sizeof(int)) &&
		memcmp(payload.magic, bootstrapMagic, sizeof(payload.magic)) == 0 &&
		payload.abiVersion == DARLING_LIFECYCLE_COHORT_ABI_VERSION &&
		validEnvelope(payload.envelope);
	struct stat status = {};
	if (!valid || output.directoryFD < 0 ||
		fstat(output.directoryFD, &status) != 0 || !S_ISDIR(status.st_mode)) {
		if (output.directoryFD >= 0)
			close(output.directoryFD);
		output.directoryFD = -1;
		return output;
	}
	output.envelope = payload.envelope;
	return output;
}

bool installLifecycleEnvelope(const darling_lifecycle_cohort_bootstrap& envelope) {
	if (!validEnvelope(envelope)) {
		errno = EINVAL;
		return false;
	}
	char controlName[DARLING_LIFECYCLE_CONTROL_NAME_CAPACITY] = {};
	char nonce[DARLING_LIFECYCLE_NONCE_HEX_BYTES + 1] = {};
	memcpy(controlName, envelope.control_name, envelope.control_name_len);
	memcpy(nonce, envelope.nonce_hex, DARLING_LIFECYCLE_NONCE_HEX_BYTES);
	if (setenv("DARLING_LIFECYCLE_CONTROL_NAME", controlName, 1) != 0)
		return false;
	if (setenv("DARLING_LIFECYCLE_CONTROL_NONCE", nonce, 1) != 0) {
		unsetenv("DARLING_LIFECYCLE_CONTROL_NAME");
		return false;
	}
	return true;
}

}
