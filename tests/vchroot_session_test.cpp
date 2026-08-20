#include <darlingserver/vchroot-session.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>

extern "C" bool darling_lifecycle_cohort_admission_open(
	struct darling_lifecycle_cohort_controller*)
{
	return true;
}

extern "C" int darling_lifecycle_guest_namespace_directory(
	struct darling_lifecycle_cohort_controller*)
{
	return -1;
}

static void require(bool condition, const char* message)
{
	if (!condition)
		throw std::runtime_error(message);
}

template <typename Operation>
static void requireError(int expected, const char* message, const Operation& operation)
{
	try {
		operation();
	} catch (const std::system_error& error) {
		require(error.code().value() == expected, message);
		return;
	}
	throw std::runtime_error(message);
}

int main()
{
	char fixture[] = "/tmp/dserver-vchroot-session.XXXXXX";
	char* parentPath = mkdtemp(fixture);
	require(parentPath != nullptr, "mkdtemp");
	const std::string prefix = std::string(parentPath) + "/prefix";
	const std::string workdir = prefix + ".workdir";
	require(mkdir(prefix.c_str(), 0700) == 0, "mkdir prefix");
	require(mkdir(workdir.c_str(), 0700) == 0, "mkdir workdir");
	const int parentFD = open(parentPath, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	const int prefixFD = open(prefix.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	const int workdirFD = open(workdir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	require(parentFD >= 0 && prefixFD >= 0 && workdirFD >= 0, "open fixture");

	auto authority = DarlingServer::VchrootSessionAuthority::create(
		prefixFD, parentFD, "prefix", workdirFD, "prefix.workdir", 41, nullptr);
	require(authority->generation() == 41, "generation");
	auto initial = authority->issue();
	// Process owns a shared capability: fork inherits the same session object,
	// exec preserves it in the existing Process, and parent destruction cannot
	// revoke a surviving child's authority.
	auto forkChild = initial;
	auto execProcess = forkChild;
	initial.reset();
	const int afterParentExit = forkChild->duplicateForTransfer();
	require(afterParentExit >= 0, "fork child lost capability after parent exit");
	close(afterParentExit);
	const int afterExec = execProcess->duplicateForTransfer();
	require(afterExec >= 0, "exec lost process capability");
	close(afterExec);
	auto initialAgain = authority->issue();
	const auto identity = initialAgain->identity();
	const int first = initialAgain->duplicateForTransfer();
	const int second = initialAgain->duplicateForTransfer();
	struct stat firstStatus{}, secondStatus{};
	require(first != second, "repeated requests own distinct descriptors");
	require(fstat(first, &firstStatus) == 0 && fstat(second, &secondStatus) == 0,
		"fstat duplicates");
	require(firstStatus.st_dev == identity.device && firstStatus.st_ino == identity.inode &&
		secondStatus.st_dev == identity.device && secondStatus.st_ino == identity.inode,
		"exact duplicate identity");
	require((fcntl(first, F_GETFD) & FD_CLOEXEC) != 0, "CLOEXEC transfer");
	close(first);
	close(second);
	const int lostResponse = initialAgain->duplicateForTransfer();
	close(lostResponse);
	const int afterLostResponse = initialAgain->duplicateForTransfer();
	require(afterLostResponse >= 0, "response loss consumed server ownership");
	close(afterLostResponse);

	const int adoptedFD = fcntl(workdirFD, F_DUPFD_CLOEXEC, 0);
	auto adopted = authority->adopt(adoptedFD);
	require(adopted->belongsTo(authority), "same-session adoption");

	const int regularFD = openat(parentFD, "regular", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	requireError(ENOTDIR, "regular descriptor accepted", [&] { authority->adopt(regularFD); });
	const std::string foreign = std::string(parentPath) + "/foreign";
	require(mkdir(foreign.c_str(), 0700) == 0, "mkdir foreign");
	const int foreignFD = open(foreign.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	requireError(EXDEV, "foreign directory accepted", [&] { authority->adopt(foreignFD); });

	const std::string moved = prefix + ".moved";
	require(rename(prefix.c_str(), moved.c_str()) == 0, "rename prefix");
	require(mkdir(prefix.c_str(), 0700) == 0, "replacement prefix");
	requireError(ESTALE, "named prefix replacement accepted",
		[&] { initialAgain->duplicateForTransfer(); });
	require(rmdir(prefix.c_str()) == 0, "remove replacement prefix");
	require(rename(moved.c_str(), prefix.c_str()) == 0, "restore prefix");

	authority->revoke();
	requireError(ESHUTDOWN, "revoked capability transferred",
		[&] { initialAgain->duplicateForTransfer(); });

	close(workdirFD);
	close(prefixFD);
	close(parentFD);
	require(unlink((std::string(parentPath) + "/regular").c_str()) == 0, "unlink regular");
	require(rmdir(foreign.c_str()) == 0, "rmdir foreign");
	require(rmdir(workdir.c_str()) == 0, "rmdir workdir");
	require(rmdir(prefix.c_str()) == 0, "rmdir prefix");
	require(rmdir(parentPath) == 0, "rmdir parent");
	std::puts("DSERVER_VCHROOT_SESSION_VALID");
}
