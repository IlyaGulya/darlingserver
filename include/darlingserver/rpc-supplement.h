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

// --- SPSC datapath ---------------------------------------------------------------------
//
// Single-producer / single-consumer ring access. Both directions share these helpers: for
// the c2s ring the guest is the producer and the server is the consumer; for the s2c ring
// it is reversed. They operate on offsets validated by dserver_ring_shm_validate(), so the
// only attacker-controlled values at runtime are head/tail (each owned by ONE side). We
// re-clamp them on every access anyway -- a corrupt index from the untrusted peer must never
// be able to make us read/write outside the ring's own slot array.
//
// Discipline (torn-write-safe, no locks). head and tail are FREE-RUNNING uint32 counters
// (never pre-masked); the slot index is (counter & (slot_count-1)). slot_count is a validated
// power of two, so the counters wrapping at 2^32 is harmless (2^32 is a multiple of any
// power-of-two slot_count, so the masked index stays continuous across the wrap).
//   * tail = total slots ever PUBLISHED; head = total slots ever CONSUMED.
//   * empty when tail == head; full when (tail - head) == slot_count.
//   * The producer writes the whole slot body, THEN does ONE release-store incrementing tail.
//     A consumer that observes the new tail therefore observes a fully written slot.
//   * The consumer reads the slot, THEN does ONE release-store incrementing head. The producer
//     treats a slot as free once head has advanced past it.
// The (tail - head) distance is itself untrusted (the peer owns one end), so we treat any
// distance > slot_count as "corrupt -> empty/full" rather than trusting it to index.
//
// Header-only inline so the guest (libc-free C), the server (C++), and the host loopback test
// all compile the IDENTICAL datapath.

// Pointer to the slot for free-running counter value `counter` within a ring whose header is
// at `ring`. slot_size/slot_count come from the validated control block.
static inline dserver_ring_slot_t* dserver_ring_slot_at(dserver_ring_t* ring, uint32_t counter, uint32_t slot_size, uint32_t slot_count) {
	uint32_t idx = counter & (slot_count - 1u);
	char* slots = (char*)ring + sizeof(dserver_ring_t);
	return (dserver_ring_slot_t*)(slots + (uint64_t)idx * slot_size);
}

// Producer: claim the next free slot and return a pointer to it WITHOUT publishing, or NULL
// if the ring is full (or the consumer's head looks corrupt). The caller writes the slot
// fields + body, then calls dserver_ring_producer_publish().
static inline dserver_ring_slot_t* dserver_ring_producer_begin(dserver_ring_t* ring, uint32_t slot_size, uint32_t slot_count) {
	uint32_t tail = __atomic_load_n(&ring->tail, __ATOMIC_RELAXED); // producer owns tail
	uint32_t head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE); // consumer owns head (untrusted)
	uint32_t used = tail - head; // wraps correctly in unsigned arithmetic
	if (used >= slot_count) {
		return (dserver_ring_slot_t*)0; // full, or a corrupt head -> refuse to produce
	}
	return dserver_ring_slot_at(ring, tail, slot_size, slot_count);
}

// Producer: publish the slot claimed by dserver_ring_producer_begin(). The single release
// store of tail is the linearization point; everything written to the slot happens-before it.
static inline void dserver_ring_producer_publish(dserver_ring_t* ring) {
	uint32_t tail = __atomic_load_n(&ring->tail, __ATOMIC_RELAXED);
	__atomic_store_n(&ring->tail, tail + 1u, __ATOMIC_RELEASE);
}

// Consumer: return a pointer to the next published slot WITHOUT consuming, or NULL if empty
// (or the producer's tail looks corrupt). The caller reads the body, then calls
// dserver_ring_consumer_advance().
static inline dserver_ring_slot_t* dserver_ring_consumer_begin(dserver_ring_t* ring, uint32_t slot_size, uint32_t slot_count) {
	uint32_t head = __atomic_load_n(&ring->head, __ATOMIC_RELAXED); // consumer owns head
	uint32_t tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE); // producer owns tail (untrusted)
	uint32_t avail = tail - head; // wraps correctly in unsigned arithmetic
	if (avail == 0u || avail > slot_count) {
		return (dserver_ring_slot_t*)0; // empty, or a corrupt tail -> consume nothing
	}
	return dserver_ring_slot_at(ring, head, slot_size, slot_count);
}

// Consumer: free the slot returned by dserver_ring_consumer_begin().
static inline void dserver_ring_consumer_advance(dserver_ring_t* ring) {
	uint32_t head = __atomic_load_n(&ring->head, __ATOMIC_RELAXED);
	__atomic_store_n(&ring->head, head + 1u, __ATOMIC_RELEASE);
}

// --- Ring message convention (P3, first migrated op) ----------------------------------
//
// A request slot carries: callnum = dserver_callnum_<name>, seq = a guest-chosen request id,
// length = inline request-body bytes, payload = the dserver_call_<name>_t body (none for a
// no-arg call like task_self_trap). A reply slot echoes the request's callnum + seq, sets
// flags bit0 if the call failed, carries the result code in dserver_ring_reply_hdr_t at the
// start of the payload, followed by the dserver_reply_<name>_t body. Big bodies (arena_len>0)
// are P4; P3 only migrates small inline-only ops.
#define DSERVER_RING_FLAG_REPLY_ERROR 0x1u

// Prefix of every REPLY payload: the RPC result code (what the UDS reply header.code carries),
// followed by the call's reply body. Kept separate from dserver_ring_slot so the slot stays a
// pure transport header and the payload stays a faithful copy of the UDS reply body.
typedef struct dserver_ring_reply_hdr {
	int32_t code; // RPC result code (0 == success); mirrors dserver_rpc_replyhdr_t.code
} dserver_ring_reply_hdr_t;

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

// --- Server-side C2S service loop (pure, bounds-safe) ---------------------------------
//
// Drain published request slots from the c2s ring, hand each to a servicer callback, and
// publish the reply onto the s2c ring. ALL of this is attacker-exposed: the guest owns the
// c2s tail and the slot bodies. We re-validate every slot before trusting it -- callnum is
// left to the servicer (it reuses call.cpp's switch), but length/seq/arena are clamped here.
//
// The servicer computes a reply: it receives the request body (already bounds-checked to lie
// within one slot) and writes a reply body + an RPC code. It returns 0 on "handled" or
// non-zero on "refuse this callnum on the ring" (the loop then drops the request and the
// guest will time out + fall back to UDS for that op). The servicer NEVER sees a pointer into
// guest memory beyond the single validated slot payload.
//
// Returns the number of requests serviced (0 if the ring was empty or every slot was corrupt).
// `*out_woke` is set nonzero if at least one reply was published (caller should wake the guest).
typedef int (*dserver_ring_servicer_t)(
	void* ctx,
	uint32_t callnum,
	const void* req_body, uint32_t req_len,
	void* reply_body, uint32_t reply_cap, uint32_t* out_reply_len,
	int32_t* out_code
);

static inline uint32_t dserver_ring_service_c2s(
	dserver_ring_t* c2s, dserver_ring_t* s2c,
	uint32_t slot_size, uint32_t slot_count,
	dserver_ring_servicer_t servicer, void* ctx,
	int* out_woke
) {
	uint32_t serviced = 0;
	uint32_t inline_cap = slot_size - (uint32_t)sizeof(dserver_ring_slot_t);
	if (out_woke) *out_woke = 0;

	for (;;) {
		dserver_ring_slot_t* req = dserver_ring_consumer_begin(c2s, slot_size, slot_count);
		if (!req) {
			break; // empty or corrupt tail -> stop (corrupt is handled defensively by begin())
		}

		// Copy the slot's transport header out before trusting it -- the guest can mutate the
		// page concurrently. Then bounds-check the inline request body strictly.
		uint32_t callnum = req->callnum;
		uint32_t reqlen  = req->length;
		uint32_t seq     = req->seq;
		const void* req_body = (const char*)req + sizeof(dserver_ring_slot_t);
		if (reqlen > inline_cap) {
			// malformed: claimed body exceeds the slot. Drop it (consume so we don't spin) and
			// keep going; the guest op will UDS-fall-back on timeout.
			dserver_ring_consumer_advance(c2s);
			continue;
		}

		// Produce the reply into an s2c slot. If the reply ring is full we stop (leave the
		// request unconsumed so we retry it on the next wake -- backpressure, not loss).
		dserver_ring_slot_t* rep = dserver_ring_producer_begin(s2c, slot_size, slot_count);
		if (!rep) {
			break;
		}
		char* rep_payload = (char*)rep + sizeof(dserver_ring_slot_t);
		uint32_t rep_cap = inline_cap;
		if (rep_cap < (uint32_t)sizeof(dserver_ring_reply_hdr_t)) {
			// slot too small to even carry a reply header -- impossible after validate(), but
			// be defensive: drop the request.
			dserver_ring_consumer_advance(c2s);
			continue;
		}
		dserver_ring_reply_hdr_t* rhdr = (dserver_ring_reply_hdr_t*)rep_payload;
		void* rep_body = rep_payload + sizeof(dserver_ring_reply_hdr_t);
		uint32_t rep_body_cap = rep_cap - (uint32_t)sizeof(dserver_ring_reply_hdr_t);
		uint32_t rep_body_len = 0;
		int32_t code = 0;

		int handled = servicer(ctx, callnum, req_body, reqlen, rep_body, rep_body_cap, &rep_body_len, &code);

		// We are done reading the request slot now; free it.
		dserver_ring_consumer_advance(c2s);

		if (handled != 0) {
			// servicer refused this callnum on the ring; don't publish a reply (guest UDS-falls
			// back). Roll back the reply slot we hadn't published yet -- producer_begin didn't
			// move tail, so simply not publishing is the rollback.
			continue;
		}

		if (rep_body_len > rep_body_cap) {
			rep_body_len = rep_body_cap; // servicer bug guard; never overruns the slot
		}
		rhdr->code = code;
		rep->callnum = callnum;
		rep->seq = seq;
		rep->length = (uint32_t)sizeof(dserver_ring_reply_hdr_t) + rep_body_len;
		rep->arena_off = 0;
		rep->arena_len = 0;
		rep->flags = (code != 0) ? DSERVER_RING_FLAG_REPLY_ERROR : 0u;
		dserver_ring_producer_publish(s2c);
		if (out_woke) *out_woke = 1;
		++serviced;
	}

	return serviced;
}

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

// Server-side ring_attach decision: given the memfd the guest passed, its claimed size, and
// the SCM-credentialed nsid, decide whether to accept the ring. This is the bridge between
// the pure validator above and real kernel state -- it fstats the fd for its TRUE size
// (never trusting the guest's mapping_size), maps it read-only just to read the control
// block, copies the control block OUT (so validation runs on a stable server-owned copy, not
// the page the guest can mutate), validates, and unmaps. On success it hands back the real
// size and the validated control-block copy via out-params so the caller can keep the
// mapping; on any failure it returns the reject reason and maps nothing lasting.
//
// Header-only + dependency-free (only <sys/mman.h>, <sys/stat.h>, <unistd.h>, <string.h>) so
// the adversarial test drives it with a real memfd, exactly as the server will. Returns
// dserver_ring_ok or a reject reason; *out_real_size / *out_cb are written only on ok.
#if defined(__linux__) && !defined(DSERVER_RING_NO_ATTACH_CHECK)

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

static inline dserver_ring_reject_t dserver_ring_attach_check(
	int ring_fd,             // the memfd the guest sent via @fd
	uint64_t claimed_size,   // mapping_size the guest claimed in the RPC (cross-checked, not trusted)
	int32_t scm_nsid,        // the thread nsid from the kernel
	uint64_t* out_real_size, // [out] the fstat size, written on ok
	dserver_ring_shm_t* out_cb // [out] validated copy of the control block, written on ok
) {
	struct stat st;
	if (fstat(ring_fd, &st) != 0) return dserver_ring_reject_total_size;
	uint64_t real_size = (uint64_t)st.st_size;

	// the guest's claimed size must match the real fd size (a lie is a protocol violation),
	// and must be at least the control block + within the hard cap before we even map it.
	if (claimed_size != real_size) return dserver_ring_reject_total_size;
	if (real_size < sizeof(dserver_ring_shm_t) || real_size > DSERVER_RING_MAX_TOTAL_SIZE) return dserver_ring_reject_total_size;

	// map read-only just to read the header; the real attach (caller) will map RW after ok.
	void* map = mmap(NULL, sizeof(dserver_ring_shm_t), PROT_READ, MAP_SHARED, ring_fd, 0);
	if (map == MAP_FAILED) return dserver_ring_reject_total_size;

	// copy the control block out of the shared page IMMEDIATELY -- never validate in place,
	// the guest can race a write between any two reads.
	dserver_ring_shm_t cb;
	memcpy(&cb, map, sizeof(cb));
	munmap(map, sizeof(dserver_ring_shm_t));

	dserver_ring_reject_t r = dserver_ring_shm_validate(&cb, real_size, scm_nsid);
	if (r != dserver_ring_ok) return r;

	if (out_real_size) *out_real_size = real_size;
	if (out_cb) *out_cb = cb;
	return dserver_ring_ok;
}

#endif // __linux__ && !DSERVER_RING_NO_ATTACH_CHECK

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
