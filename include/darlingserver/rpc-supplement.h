#ifndef _DARLINGSERVER_RPC_SUPPLEMENT_H_
#define _DARLINGSERVER_RPC_SUPPLEMENT_H_

#include <stdint.h>
#include <stdbool.h>

#if __cplusplus
extern "C" {
#endif

//
// kqueue channels
//

/**
 * kqchan is short for "kqueue channel".
 *
 * kqueue channels are used to allow libkqueue to monitor events
 * that occur on the server side (in darlingserver). This is necessary
 * for filters like EVFILT_MACHPORT and EVFILT_PROC.
 *
 * The general process works like this:
 *   1. libkqueue is told to add a knote for one of the special filters that requires a kqchan.
 *   2. libkqueue hands that off to our filter handler code for that filter.
 *   3. our filter handler opens a kqchan using a darlingserver RPC call.
 *   4. darlingserver sets up the kqchan on the server side and sends back a socket
 *      that the client can monitor and send channel messages to.
 *   5. when the client needs to modify some of the server state for the channel,
 *      it sends a special message (specialized depending on the channel type).
 *   6. when the server receives an event of interest, it notifies the client
 *      by sending a generic notification message (to which the client should NOT reply).
 *   7. when the client receives a notification, libkqueue will attempt to read the message
 *      by calling the copyout method on the filter. for kqchan-based filters, this means
 *      sending a special message (specialized depending on the channel type) to which the
 *      server replies with the necessary event information.
 */

enum dserver_kqchan_msgnum {
	dserver_kqchan_msgnum_invalid = 0,

	/**
	 * Indicates that something has occurred on the server side that the client should know about.
	 *
	 * This is the only type of server-initiated message.
	 *
	 * This is common to all types of kqueue channels.
	 *
	 * This message does NOT require a reply.
	 */
	dserver_kqchan_msgnum_notification,

	/**
	 * A request to modify the server context for this mach port kqueue channel.
	 */
	dserver_kqchan_msgnum_mach_port_modify,

	/**
	 * A request to read the data for the most recent notification on this mach port kqueue channel.
	 */
	dserver_kqchan_msgnum_mach_port_read,

	/**
	 * A request to modify the server context for this proc kqueue channel.
	 */
	dserver_kqchan_msgnum_proc_modify,

	/**
	 * A request to read the data for the most recent notification on this proc kqueue channel.
	 */
	dserver_kqchan_msgnum_proc_read,
};

typedef enum dserver_kqchan_msgnum dserver_kqchan_msgnum_t;

typedef struct dserver_kqchan_callhdr {
	dserver_kqchan_msgnum_t number;
	int pid;
	int tid;
} dserver_kqchan_callhdr_t;

typedef struct dserver_kqchan_replyhdr {
	dserver_kqchan_msgnum_t number;
	int code;
} dserver_kqchan_replyhdr_t;

typedef struct dserver_kqchan_call_notification {
	dserver_kqchan_callhdr_t header;
} dserver_kqchan_call_notification_t;

typedef struct dserver_kqchan_call_mach_port_modify {
	dserver_kqchan_callhdr_t header;
	uint64_t receive_buffer;
	uint64_t receive_buffer_size;
	uint64_t saved_filter_flags;
} dserver_kqchan_call_mach_port_modify_t;

typedef struct dserver_kqchan_reply_mach_port_modify {
	dserver_kqchan_replyhdr_t header;
} dserver_kqchan_reply_mach_port_modify_t;

typedef struct dserver_kqchan_call_mach_port_read {
	dserver_kqchan_callhdr_t header;
	uint64_t default_buffer;
	uint64_t default_buffer_size;
} dserver_kqchan_call_mach_port_read_t;

typedef struct dserver_kqchan_reply_mach_port_read {
	dserver_kqchan_replyhdr_t header;
	struct {
		uint64_t ident;
		int16_t filter;
		uint16_t flags;
		int32_t qos;
		uint64_t udata;
		uint32_t fflags;
		uint32_t xflags;
		int64_t data;
		uint64_t ext[4];
	} kev;
} dserver_kqchan_reply_mach_port_read_t;

typedef struct dserver_kqchan_call_proc_modify {
	dserver_kqchan_callhdr_t header;
	uint32_t flags;
} dserver_kqchan_call_proc_modify_t;

typedef struct dserver_kqchan_reply_proc_modify {
	dserver_kqchan_replyhdr_t header;
} dserver_kqchan_reply_proc_modify_t;

typedef struct dserver_kqchan_call_proc_read {
	dserver_kqchan_callhdr_t header;
} dserver_kqchan_call_proc_read_t;

typedef struct dserver_kqchan_reply_proc_read {
	dserver_kqchan_replyhdr_t header;
	uint32_t fflags;
	int64_t data;
} dserver_kqchan_reply_proc_read_t;

//
// perf #18 (bead dar-dar6x4-perf-5dq.30): shared-memory SPSC ring transport ABI.
//
// The hot-path RPC round-trip (guest sendmsg -> server -> guest recvmsg) is ~90% empty
// cross-process wakeup latency, not server work (memory dar-perf-startup-rpc-90pct-empty-
// roundtrip). This is the wire format for moving small, reply-bearing, fd-free C2S calls
// onto a shared-memory ring with a futex wake, keeping UDS for connect/auth/fd-passing/
// fallback. See ~/work/dar-6x4-repro/PERF18-SHMEM-RING-ABI.md for the full design.
//
// THE GUEST IS UNTRUSTED. Every field below is written by the guest into shared memory and
// may change at any time; the server copies-in and validates before use, and NEVER
// dereferences a guest-supplied pointer (offsets only, bounds-checked). The pure validator
// dserver_ring_shm_validate() at the bottom is the trust-boundary gate; it is compiled
// identically into the server and into the adversarial unit test.
//
// This whole block is gated behind DSERVER_RING_TRANSPORT so a default build is
// byte-identical (the structs/inline fn are only referenced when the feature is enabled).
//

#ifdef DSERVER_RING_TRANSPORT

#define DSERVER_RING_MAGIC       0x44524e47u /* 'DRNG' */
#define DSERVER_RING_ABI_VERSION 1u

// Bounds the server is willing to accept for a guest-proposed ring, so a malicious or buggy
// guest can't make the server map an absurd region or compute a huge index. Kept modest;
// raise deliberately if a real workload needs deeper rings.
#define DSERVER_RING_MIN_SLOT_SIZE   64u
#define DSERVER_RING_MAX_SLOT_SIZE   4096u
#define DSERVER_RING_MIN_SLOT_COUNT  2u
#define DSERVER_RING_MAX_SLOT_COUNT  1024u
#define DSERVER_RING_MAX_TOTAL_SIZE  (16u * 1024u * 1024u) /* 16 MiB cap per ring mapping */

// Per-direction ring header. head/tail live on separate cache lines (the producer owns tail,
// the consumer owns head) so the two sides never false-share. slot_size-byte slots follow.
// The header is included by both C (guest, C11 _Alignas) and C++ (server, alignas), so the
// alignment keyword is selected per language.
#if defined(__cplusplus)
	#define DSERVER_RING_ALIGN64 alignas(64)
#else
	#define DSERVER_RING_ALIGN64 _Alignas(64)
#endif
typedef struct dserver_ring {
	DSERVER_RING_ALIGN64 uint32_t head; // consumer-owned: next slot to READ
	char _pad_head[64 - sizeof(uint32_t)];
	DSERVER_RING_ALIGN64 uint32_t tail; // producer-owned: one past last PUBLISHED slot
	char _pad_tail[64 - sizeof(uint32_t)];
	// dserver_ring_slot slots[slot_count] follow here
} dserver_ring_t;

// Overlay on each slot. The inline payload carries the existing call/reply struct body;
// bodies too big for a slot use the arena (arena_len > 0).
typedef struct dserver_ring_slot {
	uint32_t length;     // inline payload bytes used (<= slot_size - sizeof(dserver_ring_slot))
	uint32_t arena_off;  // if arena_len>0: byte offset into the arena region
	uint32_t arena_len;  // if >0: payload is in the arena, this many bytes
	uint32_t seq;        // monotonic per ring; a reply echoes its request's seq
	uint32_t callnum;    // dserver_callnum_t; validated against the C2S call switch
	uint32_t flags;      // bit0 = reply-is-error; rest reserved (must be 0)
	// char payload[] follows
} dserver_ring_slot_t;

// The control block at the head of the shared mapping. The guest fills this in once before
// handing the memfd to the server; the server treats every field as adversarial input.
typedef struct dserver_ring_shm {
	uint32_t magic;        // must == DSERVER_RING_MAGIC
	uint16_t abi_version;  // must == DSERVER_RING_ABI_VERSION
	uint16_t slot_size;    // [MIN,MAX]_SLOT_SIZE; >= sizeof(dserver_ring_slot)
	uint32_t slot_count;   // power of two in [MIN,MAX]_SLOT_COUNT
	uint32_t arena_off;    // byte offset of the arena within the mapping (0 if none)
	uint32_t arena_size;   // arena bytes (0 if none)
	uint32_t c2s_ring_off; // offset of the request ring (guest->server)
	uint32_t s2c_ring_off; // offset of the reply ring (server->guest)
	uint32_t total_size;   // full mapping size; server cross-checks vs the fd's real size
	int32_t  guest_tid;    // nsid the guest claims; server cross-checks vs SCM credentials
	// Futex wake words. Plain uint32_t storage so the struct is a portable C/C++ wire format;
	// atomicity is at the ACCESS site (the producer/consumer use atomic load/store + FUTEX_*),
	// not in the storage type. Each on its own cache line so the two directions don't
	// false-share. (head/tail in dserver_ring carry the actual ring state; these only gate
	// sleep/wake.)
	DSERVER_RING_ALIGN64 uint32_t c2s_futex; // guest bumps to wake the server (via a registered eventfd)
	DSERVER_RING_ALIGN64 uint32_t s2c_futex; // server bumps to wake the guest
} dserver_ring_shm_t;

// Why a ring control block was rejected. Returned by dserver_ring_shm_validate(); the server
// logs it once and falls the thread back to UDS. (0 == accepted.)
typedef enum dserver_ring_reject {
	dserver_ring_ok = 0,
	dserver_ring_reject_magic,
	dserver_ring_reject_abi,
	dserver_ring_reject_slot_size,
	dserver_ring_reject_slot_count,      // not in range, or not a power of two
	dserver_ring_reject_total_size,      // out of [hdr, MAX], or != the fd's real size
	dserver_ring_reject_ring_bounds,     // a ring's [off, off + header + slots) escapes the mapping
	dserver_ring_reject_ring_overlap,    // c2s and s2c rings (or the header) overlap
	dserver_ring_reject_arena_bounds,    // arena [off, off+size) escapes the mapping
	dserver_ring_reject_arena_overlap,   // arena overlaps a ring or the header
	dserver_ring_reject_tid,             // guest_tid != the SCM-credentialed nsid
} dserver_ring_reject_t;

// Pure trust-boundary validator. Takes a COPY of the guest's control block (copied in by the
// caller -- never validate in place, the guest can mutate it concurrently) plus the mapping's
// real size (from fstat of the memfd) and the SCM-credentialed nsid. Returns dserver_ring_ok
// or the first reason it failed. Dereferences NOTHING -- arithmetic only -- so it is safe to
// run on fully attacker-controlled input. Uint64 math throughout so 32-bit fields can't wrap.
static inline dserver_ring_reject_t dserver_ring_shm_validate(
	const dserver_ring_shm_t* cb, // a server-owned COPY, not the shared page
	uint64_t real_mapping_size,   // fstat size of the memfd the guest passed
	int32_t  scm_nsid             // the thread nsid from the kernel, not from the guest
) {
	if (cb->magic != DSERVER_RING_MAGIC) return dserver_ring_reject_magic;
	if (cb->abi_version != DSERVER_RING_ABI_VERSION) return dserver_ring_reject_abi;

	uint64_t slot_size = cb->slot_size;
	if (slot_size < DSERVER_RING_MIN_SLOT_SIZE || slot_size > DSERVER_RING_MAX_SLOT_SIZE) return dserver_ring_reject_slot_size;
	if (slot_size < sizeof(dserver_ring_slot_t)) return dserver_ring_reject_slot_size;

	uint64_t slot_count = cb->slot_count;
	if (slot_count < DSERVER_RING_MIN_SLOT_COUNT || slot_count > DSERVER_RING_MAX_SLOT_COUNT) return dserver_ring_reject_slot_count;
	if ((slot_count & (slot_count - 1)) != 0) return dserver_ring_reject_slot_count; // power of two

	uint64_t total = cb->total_size;
	uint64_t hdr = sizeof(dserver_ring_shm_t);
	if (total < hdr || total > DSERVER_RING_MAX_TOTAL_SIZE) return dserver_ring_reject_total_size;
	if (total != real_mapping_size) return dserver_ring_reject_total_size; // the fd must be exactly this big

	// each ring spans [off, off + sizeof(header) + slot_count*slot_size)
	uint64_t ring_span = sizeof(dserver_ring_t) + slot_count * slot_size;
	uint64_t c2s_lo = cb->c2s_ring_off, c2s_hi = c2s_lo + ring_span;
	uint64_t s2c_lo = cb->s2c_ring_off, s2c_hi = s2c_lo + ring_span;
	if (c2s_lo < hdr || c2s_hi > total) return dserver_ring_reject_ring_bounds;
	if (s2c_lo < hdr || s2c_hi > total) return dserver_ring_reject_ring_bounds;
	// rings must not overlap each other
	if (c2s_lo < s2c_hi && s2c_lo < c2s_hi) return dserver_ring_reject_ring_overlap;

	if (cb->arena_size != 0) {
		uint64_t a_lo = cb->arena_off, a_hi = a_lo + cb->arena_size;
		if (a_lo < hdr || a_hi > total) return dserver_ring_reject_arena_bounds;
		// arena must not overlap either ring
		if (a_lo < c2s_hi && c2s_lo < a_hi) return dserver_ring_reject_arena_overlap;
		if (a_lo < s2c_hi && s2c_lo < a_hi) return dserver_ring_reject_arena_overlap;
	}

	if (cb->guest_tid != scm_nsid) return dserver_ring_reject_tid;

	return dserver_ring_ok;
}

#endif // DSERVER_RING_TRANSPORT

//
// S2C
//
// server-to-client RPC calls
//

enum dserver_s2c_msgnum {
	dserver_s2c_msgnum_invalid = 0,
	dserver_s2c_msgnum_mmap,
	dserver_s2c_msgnum_munmap,
	dserver_s2c_msgnum_mprotect,
	dserver_s2c_msgnum_msync,
};

typedef enum dserver_s2c_msgnum dserver_s2c_msgnum_t;

typedef struct dserver_s2c_callhdr {
	int call_number;
	dserver_s2c_msgnum_t s2c_number;
} dserver_s2c_callhdr_t;

typedef struct dserver_s2c_replyhdr {
	int call_number;
	int pid;
	int tid;
	int architecture;
	dserver_s2c_msgnum_t s2c_number;
} dserver_s2c_replyhdr_t;

typedef struct dserver_s2c_call_mmap {
	dserver_s2c_callhdr_t header;
	uint64_t address;
	uint64_t length;
	int32_t protection;
	int32_t flags;
	int32_t fd;
	int64_t offset;
} dserver_s2c_call_mmap_t;

typedef struct dserver_s2c_reply_mmap {
	dserver_s2c_replyhdr_t header;
	uint64_t address;
	int errno_result;
} dserver_s2c_reply_mmap_t;

typedef struct dserver_s2c_call_munmap {
	dserver_s2c_callhdr_t header;
	uint64_t address;
	uint64_t length;
} dserver_s2c_call_munmap_t;

typedef struct dserver_s2c_reply_munmap {
	dserver_s2c_replyhdr_t header;
	int return_value;
	int errno_result;
} dserver_s2c_reply_munmap_t;

typedef struct dserver_s2c_call_mprotect {
	dserver_s2c_callhdr_t header;
	uint64_t address;
	uint64_t length;
	int protection;
} dserver_s2c_call_mprotect_t;

typedef struct dserver_s2c_reply_mprotect {
	dserver_s2c_replyhdr_t header;
	int return_value;
	int errno_result;
} dserver_s2c_reply_mprotect_t;

typedef struct dserver_s2c_call_msync {
	dserver_s2c_callhdr_t header;
	uint64_t address;
	uint64_t size;
	int sync_flags;
} dserver_s2c_call_msync_t;

typedef struct dserver_s2c_reply_msync {
	dserver_s2c_replyhdr_t header;
	int return_value;
	int errno_result;
} dserver_s2c_reply_msync_t;

typedef union dserver_s2c_call {
	dserver_s2c_call_mmap_t mmap;
	dserver_s2c_call_munmap_t munmap;
	dserver_s2c_call_mprotect_t mprotect;
        dserver_s2c_call_msync_t msync;
} dserver_s2c_call_t;

#if __cplusplus
};
#endif

#endif // _DARLINGSERVER_RPC_SUPPLEMENT_H_
