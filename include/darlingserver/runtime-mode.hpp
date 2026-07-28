#pragma once

#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace DarlingServer {

enum class RuntimeMode {
	PrivilegedOverlay,
	PrivilegedCopy,
	PrivilegedEunion,
	RootlessEunion,
};

class RuntimeModeError : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

RuntimeMode requireRuntimeModeFromEnvironment(bool eunionCapable);
std::string_view runtimeModeName(RuntimeMode mode);
bool runtimeModeIsRootless(RuntimeMode mode);
bool runtimeModeUsesOverlay(RuntimeMode mode);
bool runtimeModeUsesEunion(RuntimeMode mode);
void validateRuntimeModePrefixFD(
	int prefixFD,
	int parentFD,
	const char* leaf,
	int workdirFD,
	RuntimeMode mode
);
std::string runtimePrefixProcPath(int prefixFD);
void validateRootlessProcessCredentials(uid_t expectedUID, gid_t expectedGID);

}
