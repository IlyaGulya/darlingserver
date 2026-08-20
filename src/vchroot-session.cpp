#include <darlingserver/vchroot-session.hpp>

#ifdef DARLING_LIFECYCLE_COHORT_V1
#include <darling_lifecycle_cohort.h>
#endif

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>

namespace DarlingServer {

static VchrootObjectIdentity inspectDirectoryIdentity(int descriptor, const char* what)
{
	struct stat status;
	if (fstat(descriptor, &status) != 0)
		throw std::system_error(errno, std::generic_category(), what);
	if (!S_ISDIR(status.st_mode))
		throw std::system_error(ENOTDIR, std::generic_category(), what);
	return {status.st_dev, status.st_ino};
}

static FD duplicateDescriptor(int descriptor, const char* what)
{
	const int duplicate = fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
	if (duplicate < 0)
		throw std::system_error(errno, std::generic_category(), what);
	return FD(duplicate);
}

VchrootDirectoryCapability::VchrootDirectoryCapability(
	FD directory,
	VchrootObjectIdentity identity,
	std::shared_ptr<VchrootSessionAuthority> authority
) :
	_directory(std::move(directory)),
	_identity(identity),
	_authority(std::move(authority))
{
}

int VchrootDirectoryCapability::duplicateForTransfer() const
{
	std::lock_guard lock(_lock);
	if (!_authority)
		throw std::system_error(ESHUTDOWN, std::generic_category(),
			"vchroot session revoked");
	_authority->validateCapability(_directory.fd(), _identity);
	return duplicateDescriptor(_directory.fd(), "duplicate retained vchroot directory").extract();
}

uint64_t VchrootDirectoryCapability::generation() const noexcept
{
	return _authority ? _authority->generation() : 0;
}

VchrootObjectIdentity VchrootDirectoryCapability::identity() const noexcept
{
	return _identity;
}

bool VchrootDirectoryCapability::belongsTo(
	const std::shared_ptr<VchrootSessionAuthority>& authority
) const noexcept
{
	return _authority == authority;
}

std::shared_ptr<VchrootSessionAuthority> VchrootSessionAuthority::create(
	int prefixFD,
	int parentFD,
	const char* prefixLeaf,
	int directoryFD,
	const char* directoryLeaf,
	uint64_t generation,
	::darling_lifecycle_cohort_controller* controller
)
{
	if (generation == 0)
		throw std::invalid_argument("zero vchroot session generation");
	if (!prefixLeaf || !*prefixLeaf || strnlen(prefixLeaf, NAME_MAX + 1) > NAME_MAX ||
		strchr(prefixLeaf, '/') ||
		(!controller && (!directoryLeaf || !*directoryLeaf)) ||
		(directoryLeaf && (strnlen(directoryLeaf, NAME_MAX + 1) > NAME_MAX ||
			strchr(directoryLeaf, '/'))))
		throw std::invalid_argument("invalid retained prefix leaf");
	auto prefix = duplicateDescriptor(prefixFD, "duplicate retained prefix");
	auto parent = duplicateDescriptor(parentFD, "duplicate retained prefix parent");
	auto directory = duplicateDescriptor(directoryFD, "duplicate retained vchroot root");
	std::array<char, NAME_MAX + 1> leaf{};
	strncpy(leaf.data(), prefixLeaf, NAME_MAX);
	std::array<char, NAME_MAX + 1> rootLeaf{};
	if (directoryLeaf)
		strncpy(rootLeaf.data(), directoryLeaf, NAME_MAX);
	const auto prefixIdentity = inspectDirectoryIdentity(prefix.fd(), "validate retained prefix");
	const auto rootIdentity = inspectDirectoryIdentity(directory.fd(), "validate retained vchroot root");
	return std::shared_ptr<VchrootSessionAuthority>(new VchrootSessionAuthority(
		std::move(prefix), std::move(parent), leaf, std::move(directory), rootLeaf,
		prefixIdentity, rootIdentity,
		generation, controller));
}

VchrootSessionAuthority::VchrootSessionAuthority(
	FD prefix,
	FD parent,
	std::array<char, NAME_MAX + 1> prefixLeaf,
	FD directory,
	std::array<char, NAME_MAX + 1> directoryLeaf,
	VchrootObjectIdentity prefixIdentity,
	VchrootObjectIdentity directoryIdentity,
	uint64_t generation,
	::darling_lifecycle_cohort_controller* controller
) :
	_prefix(std::move(prefix)),
	_parent(std::move(parent)),
	_prefixLeaf(prefixLeaf),
	_directory(std::move(directory)),
	_directoryLeaf(directoryLeaf),
	_prefixIdentity(prefixIdentity),
	_directoryIdentity(directoryIdentity),
	_generation(generation),
	_controller(controller)
{
}

VchrootSessionAuthority::~VchrootSessionAuthority()
{
	revoke();
}

void VchrootSessionAuthority::validateAdmission() const
{
	if (!_active.load(std::memory_order_acquire))
		throw std::system_error(ESHUTDOWN, std::generic_category(),
			"vchroot session revoked");
	if (_controller &&
#ifdef DARLING_LIFECYCLE_COHORT_V1
		!darling_lifecycle_cohort_admission_open(_controller)
#else
		true
#endif
	)
		throw std::system_error(ESHUTDOWN, std::generic_category(),
			"lifecycle controller revoked");
	const auto prefix = inspectDirectoryIdentity(_prefix.fd(), "revalidate retained prefix");
	if (prefix.device != _prefixIdentity.device || prefix.inode != _prefixIdentity.inode)
		throw std::system_error(ESTALE, std::generic_category(),
			"retained prefix identity mismatch");
	struct stat namedPrefix;
	if (fstatat(_parent.fd(), _prefixLeaf.data(), &namedPrefix,
			AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(namedPrefix.st_mode) ||
		namedPrefix.st_dev != _prefixIdentity.device ||
		namedPrefix.st_ino != _prefixIdentity.inode)
		throw std::system_error(ESTALE, std::generic_category(),
			"named prefix identity mismatch");
	if (_controller) {
#ifdef DARLING_LIFECYCLE_COHORT_V1
		const int authoritative = darling_lifecycle_guest_namespace_directory(_controller);
		if (authoritative < 0)
			throw std::system_error(ESHUTDOWN, std::generic_category(),
				"runtime lower authority unavailable");
		FD retained(authoritative);
		const auto observed = inspectDirectoryIdentity(
			retained.fd(), "revalidate controller runtime lower");
		if (observed.device != _directoryIdentity.device ||
			observed.inode != _directoryIdentity.inode)
			throw std::system_error(ESTALE, std::generic_category(),
				"controller runtime lower identity mismatch");
#else
		throw std::system_error(ESHUTDOWN, std::generic_category(),
			"lifecycle controller unavailable in OFF build");
#endif
	} else {
		struct stat namedDirectory;
		if (fstatat(_parent.fd(), _directoryLeaf.data(), &namedDirectory,
				AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(namedDirectory.st_mode) ||
			namedDirectory.st_dev != _directoryIdentity.device ||
			namedDirectory.st_ino != _directoryIdentity.inode)
			throw std::system_error(ESTALE, std::generic_category(),
				"named vchroot identity mismatch");
	}
}

void VchrootSessionAuthority::validateDirectory(int descriptor) const
{
	const auto observed = inspectDirectoryIdentity(descriptor, "validate vchroot directory");
	if (observed.device != _directoryIdentity.device ||
		observed.inode != _directoryIdentity.inode)
		throw std::system_error(EXDEV, std::generic_category(),
			"cross-session vchroot directory");
}

void VchrootSessionAuthority::validateCapability(
	int descriptor,
	VchrootObjectIdentity identity
) const
{
	std::lock_guard lock(_lock);
	validateAdmission();
	const auto observed = inspectDirectoryIdentity(
		descriptor, "validate retained vchroot directory");
	if (observed.device != identity.device || observed.inode != identity.inode ||
		identity.device != _directoryIdentity.device ||
		identity.inode != _directoryIdentity.inode)
		throw std::system_error(ESTALE, std::generic_category(),
			"vchroot directory identity mismatch");
}

std::shared_ptr<VchrootDirectoryCapability> VchrootSessionAuthority::issue()
{
	std::lock_guard lock(_lock);
	validateAdmission();
	validateDirectory(_directory.fd());
	return std::shared_ptr<VchrootDirectoryCapability>(new VchrootDirectoryCapability(
		duplicateDescriptor(_directory.fd(), "issue vchroot capability"),
		_directoryIdentity, shared_from_this()));
}

std::shared_ptr<VchrootDirectoryCapability> VchrootSessionAuthority::adopt(int descriptor)
{
	FD incoming(descriptor);
	std::lock_guard lock(_lock);
	validateAdmission();
	validateDirectory(incoming.fd());
	return std::shared_ptr<VchrootDirectoryCapability>(new VchrootDirectoryCapability(
		std::move(incoming), _directoryIdentity, shared_from_this()));
}

void VchrootSessionAuthority::revoke() noexcept
{
	_active.store(false, std::memory_order_release);
}

bool VchrootSessionAuthority::active() const noexcept
{
	if (!_active.load(std::memory_order_acquire))
		return false;
	if (!_controller)
		return true;
#ifdef DARLING_LIFECYCLE_COHORT_V1
	return darling_lifecycle_cohort_admission_open(_controller);
#else
	return false;
#endif
}

uint64_t VchrootSessionAuthority::generation() const noexcept
{
	return _generation;
}

VchrootObjectIdentity VchrootSessionAuthority::directoryIdentity() const noexcept
{
	return _directoryIdentity;
}

} // namespace DarlingServer
