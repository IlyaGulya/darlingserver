#include <darlingserver/test-diagnostics.hpp>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unistd.h>
#include <fcntl.h>

namespace DarlingServer::TestDiagnostics {

static constexpr const char* TraceEnv = "DSERVER_TEST_TRACE_FILE";
static constexpr const char* FaultEnv = "DSERVER_TEST_FAULT_FILE";
static std::mutex traceMutex;

static const char* semaphoreOutcome(int resultCode) {
	switch (resultCode) {
		case 0:
			return "success";
		case 14: // KERN_ABORTED
			return "interrupted";
		case 49: // KERN_OPERATION_TIMED_OUT
			return "timeout";
		default:
			return "error";
	}
}

static std::string semaphoreNames(unsigned int waitName, bool hasSignalName, unsigned int signalName) {
	std::string names = " wait_name=" + std::to_string(waitName) + " signal_name=";
	return names + (hasSignalName ? std::to_string(signalName) : "none");
}

bool enabled() {
	return std::getenv(TraceEnv) != nullptr;
}

bool consumeFault(const char* name) {
	const char* path = std::getenv(FaultEnv);
	if (!path || path[0] == '\0' || !name || name[0] == '\0') {
		return false;
	}

	int fd = open(path, O_RDONLY | O_CLOEXEC);
	std::string hostPath;
	if (fd < 0 && path[0] == '/') {
		if (const char* prefix = std::getenv("DPREFIX"); prefix && prefix[0] != '\0') {
			hostPath = std::string(prefix) + path;
			fd = open(hostPath.c_str(), O_RDONLY | O_CLOEXEC);
			if (fd < 0) {
				hostPath = std::string(prefix) + "/libexec/darling" + path;
				fd = open(hostPath.c_str(), O_RDONLY | O_CLOEXEC);
			}
		}
	}
	if (fd < 0) {
		return false;
	}

	char buffer[128] = {};
	ssize_t count = read(fd, buffer, sizeof(buffer) - 1);
	close(fd);
	if (count <= 0) {
		return false;
	}

	while (count > 0 && (buffer[count - 1] == '\n' || buffer[count - 1] == '\r' || buffer[count - 1] == ' ' || buffer[count - 1] == '\t')) {
		buffer[--count] = '\0';
	}

	if (std::strcmp(buffer, name) != 0) {
		return false;
	}

	if (!hostPath.empty()) {
		unlink(hostPath.c_str());
	} else {
		unlink(path);
	}
	traceLine(std::string("test_fault.consume name=") + name);
	return true;
}

void traceLine(const std::string& line) {
	const char* path = std::getenv(TraceEnv);
	if (!path || path[0] == '\0') {
		return;
	}

	std::string record = line;
	record.push_back('\n');

	std::lock_guard lock(traceMutex);
	int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
	if (fd < 0) {
		return;
	}

	const char* cursor = record.data();
	size_t remaining = record.size();
	while (remaining > 0) {
		ssize_t written = write(fd, cursor, remaining);
		if (written <= 0) {
			break;
		}
		cursor += written;
		remaining -= static_cast<size_t>(written);
	}

	close(fd);
}

void traceSemaphoreCallBegin(
	const char* operation,
	int pid,
	int tid,
	unsigned int waitName,
	bool hasSignalName,
	unsigned int signalName,
	unsigned int sec,
	unsigned int nsec
) {
	traceLine(
		std::string("rpc.semaphore.begin operation=") + operation +
		" pid=" + std::to_string(pid) +
		" tid=" + std::to_string(tid) +
		semaphoreNames(waitName, hasSignalName, signalName) +
		" sec=" + std::to_string(sec) +
		" nsec=" + std::to_string(nsec)
	);
}

void traceSemaphoreCallReply(
	const char* operation,
	int pid,
	int tid,
	unsigned int waitName,
	bool hasSignalName,
	unsigned int signalName,
	int resultCode
) {
	traceLine(
		std::string("rpc.semaphore.reply operation=") + operation +
		" pid=" + std::to_string(pid) +
		" tid=" + std::to_string(tid) +
		semaphoreNames(waitName, hasSignalName, signalName) +
		" code=" + std::to_string(resultCode) +
		" outcome=" + semaphoreOutcome(resultCode) +
		" terminal=reply-enqueued"
	);
}

}
