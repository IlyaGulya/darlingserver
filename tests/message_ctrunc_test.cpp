#include <darlingserver/message.hpp>
#include <darlingserver/metrics.hpp>

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

namespace DarlingServer {
	Metrics& Metrics::shared() {
		static Metrics metrics;
		return metrics;
	}

	uint64_t Metrics::nowMonoUs() {
		return 0;
	}
}

static void check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "%s\n", message);
		std::exit(1);
	}
}

static void fail_errno(const char* message) {
	std::perror(message);
	std::exit(1);
}

class OwnedFd {
public:
	OwnedFd() = default;
	explicit OwnedFd(int fd) : _fd(fd) {}
	~OwnedFd() {
		if (_fd >= 0) {
			close(_fd);
		}
	}

	OwnedFd(const OwnedFd&) = delete;
	OwnedFd& operator=(const OwnedFd&) = delete;

	OwnedFd(OwnedFd&& other) noexcept : _fd(other._fd) {
		other._fd = -1;
	}

	OwnedFd& operator=(OwnedFd&& other) noexcept {
		if (this != &other) {
			if (_fd >= 0) {
				close(_fd);
			}
			_fd = other._fd;
			other._fd = -1;
		}
		return *this;
	}

	int get() const {
		return _fd;
	}

private:
	int _fd = -1;
};

static std::array<OwnedFd, 2> socket_pair() {
	int fds[2];
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) != 0) {
		fail_errno("socketpair");
	}
	int passcred = 1;
	if (setsockopt(fds[1], SOL_SOCKET, SO_PASSCRED, &passcred, sizeof(passcred)) != 0) {
		fail_errno("setsockopt SO_PASSCRED");
	}
	return {OwnedFd(fds[0]), OwnedFd(fds[1])};
}

static std::vector<OwnedFd> make_fds(size_t count) {
	std::vector<OwnedFd> fds;
	fds.reserve(count);

	while (fds.size() < count) {
		int pipe_fds[2];
		if (pipe(pipe_fds) != 0) {
			fail_errno("pipe");
		}
		fds.emplace_back(pipe_fds[0]);
		if (fds.size() < count) {
			fds.emplace_back(pipe_fds[1]);
		} else {
			close(pipe_fds[1]);
		}
	}
	return fds;
}

static void send_fds(int socket, const std::vector<OwnedFd>& fds) {
	char byte = 'x';
	struct iovec iov = {
		.iov_base = &byte,
		.iov_len = sizeof(byte),
	};
	std::vector<char> control(CMSG_SPACE(sizeof(int) * fds.size()));
	struct msghdr msg = {};
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control.data();
	msg.msg_controllen = control.size();

	struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());

	std::vector<int> raw_fds;
	raw_fds.reserve(fds.size());
	for (const auto& fd : fds) {
		raw_fds.push_back(fd.get());
	}
	std::memcpy(CMSG_DATA(cmsg), raw_fds.data(), sizeof(int) * raw_fds.size());

	if (sendmsg(socket, &msg, 0) < 0) {
		fail_errno("sendmsg");
	}
}

int main() {
	constexpr size_t rpc_reply_with_sync_pipe_fds = 5;

	{
		auto sockets = socket_pair();
		DarlingServer::MessageQueue queue;
		auto fds = make_fds(rpc_reply_with_sync_pipe_fds);
		send_fds(sockets[0].get(), fds);

		(void)queue.receiveMany(sockets[1].get());
		auto message = queue.pop();
		check(message.has_value(), "message with maximum push_reply fd count was dropped");
		const size_t descriptor_count = message->descriptors().size();
		if (descriptor_count != rpc_reply_with_sync_pipe_fds) {
			std::fprintf(
				stderr,
				"message with maximum push_reply fd count was truncated: got %zu want %zu\n",
				descriptor_count,
				rpc_reply_with_sync_pipe_fds
			);
			return 1;
		}
		check(!queue.pop().has_value(), "unexpected extra message");
	}

	{
		auto sockets = socket_pair();
		DarlingServer::MessageQueue queue;
		auto fds = make_fds(32);
		send_fds(sockets[0].get(), fds);

		(void)queue.receiveMany(sockets[1].get());
		check(!queue.pop().has_value(), "truncated control message was accepted");
	}

	return 0;
}
