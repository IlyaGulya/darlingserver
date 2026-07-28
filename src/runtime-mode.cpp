#define _GNU_SOURCE 1

#include <darlingserver/runtime-mode.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace DarlingServer {

static constexpr const char* kCanonicalEnvironment = "DARLING_RUNTIME_MODE";
static constexpr const char* kMarkerName = ".darling-runtime-mode-v1";

std::string_view runtimeModeName(RuntimeMode mode)
{
	switch (mode) {
		case RuntimeMode::PrivilegedOverlay:
			return "privileged-overlay";
		case RuntimeMode::PrivilegedCopy:
			return "privileged-copy";
		case RuntimeMode::PrivilegedEunion:
			return "privileged-eunion";
		case RuntimeMode::RootlessEunion:
			return "rootless-eunion";
	}
	throw RuntimeModeError("invalid Darling runtime mode");
}

static RuntimeMode parseRuntimeMode(const char* value)
{
	if (value == nullptr)
		throw RuntimeModeError("canonical DARLING_RUNTIME_MODE is missing");
	if (std::strcmp(value, "privileged-overlay") == 0)
		return RuntimeMode::PrivilegedOverlay;
	if (std::strcmp(value, "privileged-copy") == 0)
		return RuntimeMode::PrivilegedCopy;
	if (std::strcmp(value, "privileged-eunion") == 0)
		return RuntimeMode::PrivilegedEunion;
	if (std::strcmp(value, "rootless-eunion") == 0)
		return RuntimeMode::RootlessEunion;
	throw RuntimeModeError("canonical DARLING_RUNTIME_MODE is invalid");
}

RuntimeMode requireRuntimeModeFromEnvironment(bool eunionCapable)
{
	if (std::getenv("DARLING_ROOTLESS") != nullptr ||
		std::getenv("DARLING_NOOVERLAYFS") != nullptr ||
		std::getenv("DARLING_EUNION") != nullptr)
		throw RuntimeModeError(
			"legacy runtime flags are forbidden after launcher normalization");

	RuntimeMode mode = parseRuntimeMode(std::getenv(kCanonicalEnvironment));
	if (runtimeModeUsesEunion(mode) && !eunionCapable)
		throw RuntimeModeError(
			"canonical runtime mode requires an E-UNION-capable build");
	return mode;
}

bool runtimeModeIsRootless(RuntimeMode mode)
{
	return mode == RuntimeMode::RootlessEunion;
}

bool runtimeModeUsesOverlay(RuntimeMode mode)
{
	return mode == RuntimeMode::PrivilegedOverlay;
}

bool runtimeModeUsesEunion(RuntimeMode mode)
{
	return mode == RuntimeMode::PrivilegedEunion ||
		mode == RuntimeMode::RootlessEunion;
}

void validateRootlessProcessCredentials(
	uid_t expectedUID,
	gid_t expectedGID
)
{
	if (expectedUID == 0 || expectedGID == 0)
		throw RuntimeModeError(
			"rootless mode requires non-root invoking user and group IDs");

	uid_t realUID;
	uid_t effectiveUID;
	uid_t savedUID;
	gid_t realGID;
	gid_t effectiveGID;
	gid_t savedGID;
	if (getresuid(&realUID, &effectiveUID, &savedUID) != 0 ||
		getresgid(&realGID, &effectiveGID, &savedGID) != 0)
		throw RuntimeModeError(
			std::string("cannot inspect rootless process credentials: ") +
			std::strerror(errno));
	if (realUID != expectedUID ||
		effectiveUID != expectedUID ||
		savedUID != expectedUID ||
		realGID != expectedGID ||
		effectiveGID != expectedGID ||
		savedGID != expectedGID)
		throw RuntimeModeError(
			"rootless process retained unexpected real, effective, or saved credentials");
}

void validateRuntimeModePrefixFD(
	int prefixFD,
	int parentFD,
	const char* leaf,
	int workdirFD,
	RuntimeMode mode
)
{
	if (prefixFD < 0 || parentFD < 0 || workdirFD < 0 || leaf == nullptr ||
		*leaf == '\0' || std::strcmp(leaf, ".") == 0 ||
		std::strcmp(leaf, "..") == 0 ||
		std::strchr(leaf, '/') != nullptr ||
		std::strlen(leaf) > NAME_MAX)
		throw RuntimeModeError("inherited runtime prefix handle is invalid");

	struct stat opened;
	struct stat parent;
	struct stat named;
	struct stat workdirOpened;
	struct stat workdirNamed;
	if (fstat(prefixFD, &opened) != 0 ||
		fstat(parentFD, &parent) != 0 ||
		!S_ISDIR(opened.st_mode) ||
		!S_ISDIR(parent.st_mode))
		throw RuntimeModeError(
			"inherited runtime prefix descriptors are not directories");
	if (fstatat(parentFD, leaf, &named, AT_SYMLINK_NOFOLLOW) != 0 ||
		S_ISLNK(named.st_mode) ||
		!S_ISDIR(named.st_mode) ||
		named.st_dev != opened.st_dev ||
		named.st_ino != opened.st_ino)
		throw RuntimeModeError(
			"runtime prefix name changed before darlingserver handoff");
	std::string workdirLeaf(leaf);
	workdirLeaf += ".workdir";
	if (workdirLeaf.size() > NAME_MAX ||
		fstat(workdirFD, &workdirOpened) != 0 ||
		!S_ISDIR(workdirOpened.st_mode) ||
		fstatat(
			parentFD, workdirLeaf.c_str(), &workdirNamed,
			AT_SYMLINK_NOFOLLOW) != 0 ||
		S_ISLNK(workdirNamed.st_mode) ||
		!S_ISDIR(workdirNamed.st_mode) ||
		workdirNamed.st_dev != workdirOpened.st_dev ||
		workdirNamed.st_ino != workdirOpened.st_ino)
		throw RuntimeModeError(
			"runtime prefix workdir changed before darlingserver handoff");

	int fd = openat(prefixFD, kMarkerName,
		O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		throw RuntimeModeError(
			"runtime prefix has no valid mode marker; use a new prefix");
	}

	struct stat status;
	if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
		status.st_size < 0 || status.st_size >= 128) {
		close(fd);
		throw RuntimeModeError(
			"runtime mode marker is not a bounded regular file");
	}
	char observed[128];
	ssize_t length = read(fd, observed, static_cast<size_t>(status.st_size));
	int savedErrno = errno;
	if (close(fd) != 0 && length >= 0) {
		length = -1;
		savedErrno = errno;
	}
	if (length < 0 || length != status.st_size) {
		errno = length < 0 ? savedErrno : EIO;
		throw RuntimeModeError(std::string("cannot read runtime mode marker: ") +
			std::strerror(errno));
	}
	observed[length] = '\0';
	std::string expected = "DARLING_RUNTIME_MODE_V1=";
	expected += runtimeModeName(mode);
	expected += "\n";
	if (expected != observed)
		throw RuntimeModeError(
			"runtime prefix mode mismatch; use a new prefix");
}

std::string runtimePrefixProcPath(int prefixFD)
{
	struct stat status;
	if (prefixFD < 0 || fstat(prefixFD, &status) != 0 ||
		!S_ISDIR(status.st_mode))
		throw RuntimeModeError("runtime prefix fd is not an open directory");
	return "/proc/self/fd/" + std::to_string(prefixFD);
}

}
