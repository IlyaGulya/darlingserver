#include <darlingserver/rootless-session-drain.hpp>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace DarlingServer {
namespace {

constexpr size_t childrenBudget = 1024 * 1024;
constexpr size_t childCountBudget = 65536;

int reapChildren() {
	for (;;) {
		int status = 0;
		pid_t result = waitpid(-1, &status, WNOHANG);
		if (result > 0)
			continue;
		if (result == 0 || errno == ECHILD)
			return 0;
		if (errno == EINTR)
			continue;
		return errno;
	}
}

int readChildren(std::vector<pid_t>& children) {
	int fd = open("/proc/thread-self/children", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return errno;
	std::string input;
	char buffer[4096];
	for (;;) {
		ssize_t count = read(fd, buffer, sizeof(buffer));
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0) {
			int error = errno;
			close(fd);
			return error;
		}
		if (count == 0)
			break;
		if (input.size() + static_cast<size_t>(count) > childrenBudget) {
			close(fd);
			return E2BIG;
		}
		input.append(buffer, static_cast<size_t>(count));
	}
	close(fd);

	const char* cursor = input.data();
	const char* end = cursor + input.size();
	while (cursor < end) {
		while (cursor < end && *cursor == ' ')
			++cursor;
		if (cursor == end)
			break;
		pid_t pid = 0;
		auto parsed = std::from_chars(cursor, end, pid);
		if (parsed.ec != std::errc() || pid <= 0 ||
			(parsed.ptr != end && *parsed.ptr != ' '))
			return EPROTO;
		children.push_back(pid);
		if (children.size() > childCountBudget)
			return E2BIG;
		cursor = parsed.ptr;
	}
	return 0;
}

int signalChild(pid_t pid, int signalNumber) {
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
	int pidfd = static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
	if (pidfd < 0)
		return errno == ESRCH ? 0 : errno;
	int result = static_cast<int>(
		syscall(SYS_pidfd_send_signal, pidfd, signalNumber, nullptr, 0)
	);
	int error = result == 0 || errno == ESRCH ? 0 : errno;
	close(pidfd);
	return error;
#else
	(void)pid;
	(void)signalNumber;
	return ENOTSUP;
#endif
}

int drainPhase(
	pid_t retainedControllerPID,
	int signalNumber,
	std::chrono::steady_clock::time_point deadline
) {
	for (;;) {
		int error = reapChildren();
		if (error != 0)
			return error;
		std::vector<pid_t> children;
		error = readChildren(children);
		if (error != 0)
			return error;
		children.erase(
			std::remove(children.begin(), children.end(), retainedControllerPID),
			children.end()
		);
		if (children.empty())
			return 0;
		for (pid_t child : children) {
			error = signalChild(child, signalNumber);
			if (error != 0)
				return error;
		}
		if (std::chrono::steady_clock::now() >= deadline)
			return ETIMEDOUT;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

}

int blockRootlessLifecycleTerminationSignals() {
	sigset_t terminationSignals;
	sigemptyset(&terminationSignals);
	sigaddset(&terminationSignals, SIGTERM);
	sigaddset(&terminationSignals, SIGINT);
	const int result = pthread_sigmask(SIG_BLOCK, &terminationSignals, nullptr);
	return result;
}

int drainRootlessSessionChildren(
	pid_t retainedControllerPID,
	std::chrono::milliseconds gracefulTimeout,
	std::chrono::milliseconds killTimeout
) {
	if (retainedControllerPID <= 0)
		return EINVAL;
	int error = drainPhase(
		retainedControllerPID,
		SIGTERM,
		std::chrono::steady_clock::now() + gracefulTimeout
	);
	if (error == 0)
		return 0;
	if (error != ETIMEDOUT)
		return error;
	return drainPhase(
		retainedControllerPID,
		SIGKILL,
		std::chrono::steady_clock::now() + killTimeout
	);
}

}
