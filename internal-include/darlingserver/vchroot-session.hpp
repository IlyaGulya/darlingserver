#ifndef _DARLINGSERVER_VCHROOT_SESSION_HPP_
#define _DARLINGSERVER_VCHROOT_SESSION_HPP_

#include <atomic>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <sys/types.h>
#include <limits.h>

#include <darlingserver/utility.hpp>

struct darling_lifecycle_cohort_controller;

namespace DarlingServer {

struct VchrootObjectIdentity {
	dev_t device;
	ino_t inode;
};

class VchrootSessionAuthority;

class VchrootDirectoryCapability final {
public:
	VchrootDirectoryCapability(const VchrootDirectoryCapability&) = delete;
	VchrootDirectoryCapability& operator=(const VchrootDirectoryCapability&) = delete;
	~VchrootDirectoryCapability() = default;

	int duplicateForTransfer() const;
	uint64_t generation() const noexcept;
	VchrootObjectIdentity identity() const noexcept;
	bool belongsTo(const std::shared_ptr<VchrootSessionAuthority>& authority) const noexcept;

private:
	friend class VchrootSessionAuthority;
	VchrootDirectoryCapability(
		FD directory,
		VchrootObjectIdentity identity,
		std::shared_ptr<VchrootSessionAuthority> authority
	);

	mutable std::mutex _lock;
	FD _directory;
	VchrootObjectIdentity _identity;
	std::shared_ptr<VchrootSessionAuthority> _authority;
};

class VchrootSessionAuthority final :
	public std::enable_shared_from_this<VchrootSessionAuthority> {
public:
	static std::shared_ptr<VchrootSessionAuthority> create(
		int prefixFD,
		int parentFD,
		const char* prefixLeaf,
		int directoryFD,
		const char* directoryLeaf,
		uint64_t generation,
		::darling_lifecycle_cohort_controller* controller
	);

	VchrootSessionAuthority(const VchrootSessionAuthority&) = delete;
	VchrootSessionAuthority& operator=(const VchrootSessionAuthority&) = delete;
	~VchrootSessionAuthority();

	std::shared_ptr<VchrootDirectoryCapability> issue();
	std::shared_ptr<VchrootDirectoryCapability> adopt(int directoryFD);
	void revoke() noexcept;
	bool active() const noexcept;
	uint64_t generation() const noexcept;
	VchrootObjectIdentity directoryIdentity() const noexcept;

private:
	friend class VchrootDirectoryCapability;
	VchrootSessionAuthority(
		FD prefix,
		FD parent,
		std::array<char, NAME_MAX + 1> prefixLeaf,
		FD directory,
		std::array<char, NAME_MAX + 1> directoryLeaf,
		VchrootObjectIdentity prefixIdentity,
		VchrootObjectIdentity directoryIdentity,
		uint64_t generation,
		::darling_lifecycle_cohort_controller* controller
	);

	void validateAdmission() const;
	void validateDirectory(int descriptor) const;
	void validateCapability(int descriptor, VchrootObjectIdentity identity) const;

	mutable std::mutex _lock;
	FD _prefix;
	FD _parent;
	std::array<char, NAME_MAX + 1> _prefixLeaf;
	FD _directory;
	std::array<char, NAME_MAX + 1> _directoryLeaf;
	VchrootObjectIdentity _prefixIdentity;
	VchrootObjectIdentity _directoryIdentity;
	uint64_t _generation;
	::darling_lifecycle_cohort_controller* _controller;
	std::atomic<bool> _active{true};
};

} // namespace DarlingServer

#endif
