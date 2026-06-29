/**
 * This file is part of Darling.
 *
 * Copyright (C) 2026 Darling developers
 *
 * Darling is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Darling is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Darling.  If not, see <http://www.gnu.org/licenses/>.
 */

// perf #18 (bead dar-dar6x4-perf-5dq.30): RingBuffer -- server-side owner of an attached
// shared-memory ring. See ring.hpp + ~/work/dar-6x4-repro/PERF18-SHMEM-RING-ABI.md.

#ifdef DSERVER_RING_TRANSPORT

#define _GNU_SOURCE 1
#include <darlingserver/ring.hpp>

#include <sys/mman.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cstring>

DarlingServer::RingBuffer::RingBuffer(void* map, size_t size, FD&& eventfd, const dserver_ring_shm_t& cb):
	_map(map),
	_size(size),
	_eventfd(std::move(eventfd))
{
	std::memcpy(&_cb, &cb, sizeof(_cb));
};

DarlingServer::RingBuffer::~RingBuffer() {
	if (_map && _map != MAP_FAILED) {
		munmap(_map, _size);
		_map = nullptr;
	}
	// _eventfd (FD) closes itself.
};

std::shared_ptr<DarlingServer::RingBuffer> DarlingServer::RingBuffer::attach(int ring_fd, uint64_t claimed_size, int32_t scm_nsid, dserver_ring_reject_t* outReject) {
	uint64_t realSize = 0;
	dserver_ring_shm_t cb;

	// Validate against real kernel state first: fstat true size, map RO, copy + validate the
	// control block. Nothing lasting is mapped if this rejects.
	dserver_ring_reject_t r = dserver_ring_attach_check(ring_fd, claimed_size, scm_nsid, &realSize, &cb);
	if (r != dserver_ring_ok) {
		if (outReject) *outReject = r;
		return nullptr;
	}

	// Accepted. Dup the fd so the mapping outlives the Call (which closes its own ring_fd in
	// its dtor), then map the whole region RW.
	int duped = ::dup(ring_fd);
	if (duped < 0) {
		if (outReject) *outReject = dserver_ring_reject_total_size;
		return nullptr;
	}
	FD dupedFD(duped);

	void* map = mmap(NULL, realSize, PROT_READ | PROT_WRITE, MAP_SHARED, dupedFD.fd(), 0);
	if (map == MAP_FAILED) {
		if (outReject) *outReject = dserver_ring_reject_total_size;
		return nullptr;
	}
	// the fd is no longer needed once mapped; the mapping holds its own reference.
	dupedFD = FD();

	// The wake eventfd the guest signals; nonblocking + cloexec like the server's own.
	int efd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (efd < 0) {
		munmap(map, realSize);
		if (outReject) *outReject = dserver_ring_reject_total_size;
		return nullptr;
	}

	if (outReject) *outReject = dserver_ring_ok;
	// private ctor -> can't use make_shared
	return std::shared_ptr<RingBuffer>(new RingBuffer(map, realSize, FD(efd), cb));
};

int DarlingServer::RingBuffer::eventfd() const {
	return _eventfd.fd();
};

const dserver_ring_shm_t& DarlingServer::RingBuffer::controlBlock() const {
	return _cb;
};

dserver_ring_t* DarlingServer::RingBuffer::c2sRing() const {
	return reinterpret_cast<dserver_ring_t*>(static_cast<char*>(_map) + _cb.c2s_ring_off);
};

dserver_ring_t* DarlingServer::RingBuffer::s2cRing() const {
	return reinterpret_cast<dserver_ring_t*>(static_cast<char*>(_map) + _cb.s2c_ring_off);
};

void* DarlingServer::RingBuffer::arena() const {
	if (_cb.arena_size == 0) {
		return nullptr;
	}
	return static_cast<char*>(_map) + _cb.arena_off;
};

uint32_t DarlingServer::RingBuffer::slotSize() const {
	return _cb.slot_size;
};

uint32_t DarlingServer::RingBuffer::slotCount() const {
	return _cb.slot_count;
};

#endif // DSERVER_RING_TRANSPORT
