#include <darlingserver/runtime-mode.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace DarlingServer;

static void require(bool condition, const char* message)
{
	if (!condition) {
		std::cerr << "FAIL " << message << '\n';
		std::exit(1);
	}
}

static void clearEnvironment()
{
	unsetenv("DARLING_RUNTIME_MODE");
	unsetenv("DARLING_ROOTLESS");
	unsetenv("DARLING_NOOVERLAYFS");
	unsetenv("DARLING_EUNION");
}

static void requireMode(const char* name, RuntimeMode expected)
{
	clearEnvironment();
	setenv("DARLING_RUNTIME_MODE", name, 1);
	require(requireRuntimeModeFromEnvironment(true) == expected,
		"canonical runtime mode did not propagate to darlingserver");
}

static void requireFailure(bool capable)
{
	try {
		(void)requireRuntimeModeFromEnvironment(capable);
	} catch (const RuntimeModeError&) {
		return;
	}
	require(false, "invalid runtime environment was accepted");
}

static void writeMarker(const std::filesystem::path& directory)
{
	std::ofstream output(directory / ".darling-runtime-mode-v1");
	output << "DARLING_RUNTIME_MODE_V1=rootless-eunion\n";
	require(output.good(), "write runtime mode marker fixture");
}

static void requireRejectedPrefixFD(
	int prefixFD,
	int parentFD,
	const char* leaf,
	int workdirFD,
	const char* message
)
{
	try {
		validateRuntimeModePrefixFD(
			prefixFD, parentFD, leaf, workdirFD,
			RuntimeMode::RootlessEunion);
		require(false, message);
	} catch (const RuntimeModeError&) {
	}
}

int main()
{
	requireMode("privileged-overlay", RuntimeMode::PrivilegedOverlay);
	requireMode("privileged-copy", RuntimeMode::PrivilegedCopy);
	requireMode("privileged-eunion", RuntimeMode::PrivilegedEunion);
	requireMode("rootless-eunion", RuntimeMode::RootlessEunion);

	clearEnvironment();
	requireFailure(true);
	setenv("DARLING_RUNTIME_MODE", "unknown", 1);
	requireFailure(true);
	setenv("DARLING_RUNTIME_MODE", "rootless-eunion", 1);
	requireFailure(false);
	setenv("DARLING_ROOTLESS", "1", 1);
	requireFailure(true);
	clearEnvironment();

	validateRootlessProcessCredentials(getuid(), getgid());
	try {
		validateRootlessProcessCredentials(0, getgid());
		require(false, "root user identity was accepted for rootless mode");
	} catch (const RuntimeModeError&) {
	}
	try {
		validateRootlessProcessCredentials(getuid(), 0);
		require(false, "root group identity was accepted for rootless mode");
	} catch (const RuntimeModeError&) {
	}

	char fixture[] = "/tmp/dserver-runtime-mode.XXXXXX";
	char* root = mkdtemp(fixture);
	require(root != nullptr, "mkdtemp");
	const std::filesystem::path prefix =
		std::filesystem::path(root) / "prefix";
	std::filesystem::create_directory(prefix);
	const std::filesystem::path workdir =
		std::filesystem::path(root) / "prefix.workdir";
	std::filesystem::create_directory(workdir);
	const std::string marker =
		(prefix / ".darling-runtime-mode-v1").string();
	const int parentFD =
		open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	const int prefixFD =
		open(prefix.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	const int workdirFD =
		open(workdir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	require(parentFD >= 0 && prefixFD >= 0 && workdirFD >= 0,
		"open prefix fixture descriptors");

	try {
		validateRuntimeModePrefixFD(
			prefixFD, parentFD, "prefix", workdirFD,
			RuntimeMode::RootlessEunion);
		require(false, "missing marker was accepted");
	} catch (const RuntimeModeError&) {
		require(!std::filesystem::exists(marker),
			"missing-marker validation mutated the prefix");
	}

	writeMarker(prefix);
	validateRuntimeModePrefixFD(
		prefixFD, parentFD, "prefix", workdirFD,
		RuntimeMode::RootlessEunion);
	const std::string retainedAlias = runtimePrefixProcPath(prefixFD);
	struct stat aliasStatus;
	struct stat prefixStatus;
	require(stat(retainedAlias.c_str(), &aliasStatus) == 0,
		"retained prefix alias stat");
	require(fstat(prefixFD, &prefixStatus) == 0 &&
		aliasStatus.st_dev == prefixStatus.st_dev &&
		aliasStatus.st_ino == prefixStatus.st_ino,
		"retained prefix alias does not name exact descriptor");
	try {
		validateRuntimeModePrefixFD(
			prefixFD, parentFD, "prefix", workdirFD,
			RuntimeMode::PrivilegedOverlay);
		require(false, "mismatched marker was accepted");
	} catch (const RuntimeModeError&) {
		std::ifstream input(marker);
		std::string content;
		std::getline(input, content);
		require(content == "DARLING_RUNTIME_MODE_V1=rootless-eunion",
			"mismatch validation mutated the marker");
	}

	std::filesystem::path target = std::filesystem::path(root) / "target";
	std::filesystem::create_directories(target);
	writeMarker(target);
	std::filesystem::path sentinel = target / "sentinel";
	{
		std::ofstream output(sentinel);
		output << "unchanged\n";
	}
	const std::filesystem::path parked =
		std::filesystem::path(root) / "prefix.parked";
	require(rename(prefix.c_str(), parked.c_str()) == 0,
		"park inspected prefix");
	require(symlink(target.c_str(), prefix.c_str()) == 0,
		"replace inspected prefix with symlink");
	requireRejectedPrefixFD(
		prefixFD, parentFD, "prefix", workdirFD,
		"rename-swap runtime prefix symlink was accepted");
	struct stat retainedStatus;
	require(stat(retainedAlias.c_str(), &retainedStatus) == 0 &&
		retainedStatus.st_dev == prefixStatus.st_dev &&
		retainedStatus.st_ino == prefixStatus.st_ino,
		"rename-swap redirected retained prefix alias");
	{
		std::ifstream input(sentinel);
		std::string content;
		std::getline(input, content);
		require(content == "unchanged",
			"rename-swap rejection mutated its target");
	}
	require(unlink(prefix.c_str()) == 0 &&
		rename(parked.c_str(), prefix.c_str()) == 0,
		"restore inspected prefix fixture");
	validateRuntimeModePrefixFD(
		prefixFD, parentFD, "prefix", workdirFD,
		RuntimeMode::RootlessEunion);
	const std::filesystem::path parkedWorkdir =
		std::filesystem::path(root) / "prefix.workdir.parked";
	require(rename(workdir.c_str(), parkedWorkdir.c_str()) == 0,
		"park inspected workdir");
	require(symlink(target.c_str(), workdir.c_str()) == 0,
		"replace inspected workdir with symlink");
	requireRejectedPrefixFD(
		prefixFD, parentFD, "prefix", workdirFD,
		"rename-swap runtime workdir symlink was accepted");
	require(unlink(workdir.c_str()) == 0 &&
		rename(parkedWorkdir.c_str(), workdir.c_str()) == 0,
		"restore inspected workdir fixture");
	validateRuntimeModePrefixFD(
		prefixFD, parentFD, "prefix", workdirFD,
		RuntimeMode::RootlessEunion);

	std::filesystem::remove(marker);
	require(symlink(target.c_str(), marker.c_str()) == 0,
		"create marker symlink");
	requireRejectedPrefixFD(
		prefixFD, parentFD, "prefix", workdirFD,
		"runtime marker symlink was accepted");
	require(!std::filesystem::exists(target / "mutated"),
		"marker symlink rejection mutated target");

	close(prefixFD);
	close(workdirFD);
	close(parentFD);
	std::filesystem::remove_all(target);
	std::filesystem::remove_all(prefix);
	std::filesystem::remove_all(workdir);
	std::filesystem::remove(root);
	std::cout << "DSERVER_RUNTIME_MODE_CONTRACT_OK\n";
	return 0;
}
