#ifndef _DARLINGSERVER_RING_HPP_
#define _DARLINGSERVER_RING_HPP_

// perf #18 (bead dar-dar6x4-perf-5dq.30): server-side owner of an attached shared-memory ring.
//
// A RingBuffer owns one guest-shared mapping (the dserver_ring_shm control block + the two
// SPSC rings + arena) plus the eventfd the guest signals to wake the server. It is created
// only AFTER dserver_ring_attach_check() has validated the guest's control block, so by the
// time a RingBuffer exists the layout offsets are known-good and bounded. RAII tears the
// mapping + eventfd down on destruction; the owning Thread drops its shared_ptr on death.
//
// The whole type is gated behind DSERVER_RING_TRANSPORT so a default build never references
// it (byte-identical when off).

#ifdef DSERVER_RING_TRANSPORT

#include <cstdint>
#include <cstddef>
#include <memory>

#include <darlingserver/rpc-supplement.h>
#include <darlingserver/utility.hpp>

namespace DarlingServer {
	class RingBuffer {
	private:
		void* _map = nullptr;     // the RW mapping of the guest memfd (size _size)
		size_t _size = 0;
		FD _eventfd;              // guest writes this to wake the server; the server reads to drain
		dserver_ring_shm_t _cb;   // a server-owned COPY of the validated control block (never re-read from the page for layout)

		RingBuffer(void* map, size_t size, FD&& eventfd, const dserver_ring_shm_t& cb);

	public:
		~RingBuffer();

		RingBuffer(const RingBuffer&) = delete;
		RingBuffer& operator=(const RingBuffer&) = delete;

		/**
		 * Validate + attach the guest-supplied ring memfd. Runs dserver_ring_attach_check()
		 * (fstat true size, map RO, copy + validate the control block), and on success maps
		 * the region RW and creates the wake eventfd. Returns nullptr on any rejection (the
		 * caller falls the thread back to UDS); *outReject carries the reason for logging.
		 *
		 * The fd is duped internally for the lasting mapping; the caller keeps ownership of
		 * its own ring_fd (the generated Call dtor closes it as usual).
		 */
		static std::shared_ptr<RingBuffer> attach(int ring_fd, uint64_t claimed_size, int32_t scm_nsid, dserver_ring_reject_t* outReject);

		/** The eventfd to register as a Readable Monitor; guest signals it to wake the server. */
		int eventfd() const;

		/** Accessors into the validated mapping (offsets came from the validated control block). */
		const dserver_ring_shm_t& controlBlock() const;
		dserver_ring_t* c2sRing() const;  // request ring (guest -> server)
		dserver_ring_t* s2cRing() const;  // reply ring (server -> guest)
		void* arena() const;              // nullptr if arena_size == 0
		uint32_t slotSize() const;
		uint32_t slotCount() const;

		/**
		 * Publish one reply onto the s2c ring: claim a slot, write the reply header (code) +
		 * body, echo the request's seq + callnum, single-release-store publish. Returns false
		 * if the s2c ring is full (caller leaves the guest to retry / UDS-fall-back) or the body
		 * exceeds the inline slot capacity. Does NOT wake the guest -- call wakeGuest() after a
		 * batch so one wake covers several replies.
		 */
		bool publishReply(uint32_t seq, uint32_t callnum, int32_t code, const void* body, uint32_t bodyLen);

		/**
		 * Wake a guest that is FUTEX_WAITing on the s2c futex word (bump the word, then
		 * FUTEX_WAKE). Cheap no-op cost if the guest is spinning (it just sees the new tail).
		 */
		void wakeGuest();

		/** Drain the wake eventfd (called from the Monitor callback so it stops re-firing). */
		void drainWake();
	};
};

#endif // DSERVER_RING_TRANSPORT

#endif // _DARLINGSERVER_RING_HPP_
