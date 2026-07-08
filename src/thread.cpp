/**
 * This file is part of Darling.
 *
 * Copyright (C) 2021 Darling developers
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

#include "darlingserver/registry.hpp"
#include <darlingserver/thread.hpp>
#include <darlingserver/process.hpp>
#include <darlingserver/call.hpp>
#include <darlingserver/server.hpp>
#include <darlingserver/logging.hpp>
#include <darlingserver/metrics.hpp>
#ifdef DSERVER_RING_TRANSPORT
	#include <darlingserver/ring.hpp>
	#include <darlingserver/monitor.hpp>
#endif
#include <filesystem>
#include <fstream>

#include <sys/mman.h>
#include <signal.h>

#include <darlingserver/duct-tape.h>
#include <darlingserver/duct-tape/hooks.h> // A0-ARCH stage 2c: DTAPE_XWAIT_* flags
#include <atomic>

#include <sys/syscall.h>
#include <time.h>

#if DSERVER_ASAN
	#include <sanitizer/asan_interface.h>
#endif

#include <rtsig.h>

#include <assert.h>

#include <limits>
#include <system_error>
#include <cerrno>
#include <cstdio>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <vector>

// 64KiB should be enough for us
#define THREAD_STACK_SIZE (64 * 1024ULL)
#define USE_THREAD_GUARD_PAGES 1
#define IDLE_THREAD_STACK_COUNT 8

static thread_local std::shared_ptr<DarlingServer::Thread> currentThreadVar = nullptr;
static thread_local bool returningToThreadTop = false;
static thread_local ucontext_t backToThreadTopContext;
static thread_local libsimple_lock_t* unlockMeWhenSuspending = nullptr;
static thread_local std::function<void()> currentContinuation = nullptr;
static thread_local char lastMakecontextTrace[256] = "none";

/**
 * Our microthreads use cooperative multitasking, so we don't really use interrupts per-se.
 * Rather, this is an indication to our cooperative scheduler that the microthread is doing something and
 * expects to continue to have control of the executing thread. If it calls a function/method that
 * would cause it to relinquish control of the thread, this should be considered an error.
 *
 * This is primarily of use for debugging duct-tape code and ensuring certain assumptions made in the duct-tape code hold true.
 */
static thread_local uint64_t interruptDisableCount = 0;

static bool envFlagEnabled(const char* name) {
	const char* value = getenv(name);
	return value != nullptr && value[0] != '\0' && strcmp(value, "0") != 0;
}

static bool contextTraceEnabled() {
	static const bool enabled = envFlagEnabled("DSERVER_CONTEXT_TRACE");
	return enabled;
}

extern "C" const char* dserver_last_makecontext_trace() {
	return lastMakecontextTrace;
}

static void recordMakecontextTrace(const char* branch, const DarlingServer::Thread* thread, const DarlingServer::StackPool::Stack& stack, const std::shared_ptr<DarlingServer::Call>& call) {
	auto callNumber = call ? call->number() : DarlingServer::Call::Number::Invalid;
	snprintf(lastMakecontextTrace,
			sizeof(lastMakecontextTrace),
			"branch=%s thread=%p stack=%p size=%zu call=%u(%s)",
			branch,
			static_cast<const void*>(thread),
			stack.base,
			stack.size,
			static_cast<unsigned int>(callNumber),
			DarlingServer::Call::callNumberToString(callNumber));

	if (!contextTraceEnabled()) {
		return;
	}

	fprintf(stderr, "DSERVER_CONTEXT_TRACE: %s\n", lastMakecontextTrace);
	fflush(stderr);
}

// A0-ARCH stage 0: scheduling-order fuzzer. Every A0 wake/wait bug was a 2-event reorder
// (dispatch vs wake, wake-before-suspend, stale re-run); this injects exactly that class of
// reorders on purpose so the a0-gate synth legs can find protocol holes in minutes instead
// of waiting for brew to hit them. Enabled ONLY when DSERVER_SCHED_FUZZ=<seed> is set in the
// server's environment (seed 0 disables); DSERVER_SCHED_FUZZ_RATE=<N> tunes the per-site
// injection probability to 1/N (default 16). Zero cost when disabled: one predictable branch
// on a plain bool per site.
static bool schedFuzzEnabled = false;
static uint64_t schedFuzzRate = 16;
static std::atomic<uint64_t> schedFuzzState = 0;

static void schedFuzzInit() {
	const char* seedStr = getenv("DSERVER_SCHED_FUZZ");
	if (!seedStr || !*seedStr) {
		return;
	}
	uint64_t seed = strtoull(seedStr, nullptr, 0);
	if (seed == 0) {
		return;
	}
	const char* rateStr = getenv("DSERVER_SCHED_FUZZ_RATE");
	if (rateStr && *rateStr) {
		uint64_t rate = strtoull(rateStr, nullptr, 0);
		if (rate >= 2) {
			schedFuzzRate = rate;
		}
	}
	schedFuzzState.store(seed, std::memory_order_relaxed);
	schedFuzzEnabled = true;
	fprintf(stderr, "darlingserver: sched-fuzz ENABLED seed=%llu rate=1/%llu\n",
		(unsigned long long)seed, (unsigned long long)schedFuzzRate);
}
static const bool schedFuzzInitDone = (schedFuzzInit(), true);

// deterministic per-seed xorshift64*; thread-safe via CAS so concurrent runners draw from
// one stream (cross-thread interleaving makes exact replay approximate, but the DIVERSITY
// of orderings per seed is the point, not exact replay).
static bool schedFuzzChance() {
	if (!schedFuzzEnabled) {
		return false;
	}
	uint64_t x = schedFuzzState.load(std::memory_order_relaxed);
	uint64_t next;
	do {
		next = x;
		next ^= next >> 12;
		next ^= next << 25;
		next ^= next >> 27;
	} while (!schedFuzzState.compare_exchange_weak(x, next, std::memory_order_relaxed));
	return ((next * 0x2545f4914f6cdd1dULL) >> 33) % schedFuzzRate == 0;
}

#if DSERVER_ASAN
	static thread_local void* asanOldFakeStack = nullptr;
	static thread_local const void* asanOldStackBottom = nullptr;
	static thread_local size_t asanOldStackSize = 0;
#endif

static DarlingServer::Log threadLog("thread");

DarlingServer::StackPool DarlingServer::Thread::stackPool(IDLE_THREAD_STACK_COUNT, THREAD_STACK_SIZE, USE_THREAD_GUARD_PAGES);

DarlingServer::Thread::Thread(std::shared_ptr<Process> process, NSID nsid, void* stackHint):
	_nstid(nsid),
	_process(process)
{
	_tid = -1;

	for (const auto& entry: std::filesystem::directory_iterator("/proc/" + std::to_string(process->id()) + "/task")) {
		std::ifstream statusFile(entry.path() / "status");
		std::string line;

		while (std::getline(statusFile, line)) {
			if (line.substr(0, sizeof("NSpid") - 1) == "NSpid") {
				auto pos = line.find_last_of('\t');
				std::string id;

				if (pos != line.npos) {
					id = line.substr(pos + 1);
				}

				if (id.empty()) {
					throw std::runtime_error("Failed to parse thread ID");
				}

				if (std::stoi(id) != _nstid) {
					continue;
				}

				_tid = std::stoi(entry.path().filename().string());

				break;
			}
		}
	}

	// if we can't determine the thread id from procfs, try some other more costly methods.
	if (_tid == -1) {
		std::vector<pid_t> ids;
		auto& registry = threadRegistry();
		for (const auto& entry: std::filesystem::directory_iterator("/proc/" + std::to_string(process->id()) + "/task")) {
			pid_t currentId = std::stoi(entry.path().filename().string());
			// Skip threads that are already registered, as we're sure they're not the ones we want.
			if (registry.lookupEntryByID(currentId).has_value()) {
				continue;
			}
			ids.push_back(currentId);
		}

		// we're sure this is the thread we want as this is the only unregistered thread.
		if (ids.size() == 1) {
			_tid = ids[0];
		} else if (stackHint != nullptr) {
			pid_t chosenId = -1;
			intptr_t nearest = std::numeric_limits<intptr_t>::max();

			for (auto id : ids) {
				if (ptrace(PTRACE_ATTACH, id, 0, 0) == -1) {
					continue;
				}

				int status;
				int waitStatus = waitpid(id, &status, 0);

				if (waitStatus < 0) {
					continue;
				}

				struct user_regs_struct regs;
				if (ptrace(PTRACE_GETREGS, id, 0, &regs) == -1) {
					continue;
				}

#ifdef __x86_64__
				intptr_t stackDiff = (intptr_t)stackHint - (intptr_t)regs.rsp;
				if (stackDiff >= 0 && stackDiff < nearest) {
#else
	#warning Unsupported architecture
				if (true) {
#endif
					chosenId = id;
					nearest = stackDiff;
				}

				// this is critical: we're tracing a process but cannot detach from it, and it'll not run normally.
				if (ptrace(PTRACE_DETACH, id, 0, 0) == -1) {
					throw std::system_error(errno, std::generic_category(), "Failed to detach from process.");
				}
			}

			_tid = chosenId;
		}
	}

	if (_tid == -1) {
		throw std::system_error(ESRCH, std::generic_category(), "Failed to find thread ID within darlingserver's namespace");
	}

	// NOTE: it's okay to use raw `this` without a shared pointer because the duct-taped thread will always live for less time than this Thread instance
	_dtapeThread = dtape_thread_create(process->_dtapeTask, _nstid, this);
	_s2cPerformSempahore = dtape_semaphore_create(process->_dtapeTask, 1);
	_s2cReplySempahore = dtape_semaphore_create(process->_dtapeTask, 0);
	_s2cInterruptEnterSemaphore = dtape_semaphore_create(process->_dtapeTask, 0);
	_s2cInterruptExitSemaphore = dtape_semaphore_create(process->_dtapeTask, 0);

	threadLog.info() << "New thread created with ID " << _tid << " and NSID " << _nstid << " for process with ID " << (process ? process->id() : -1) << " and NSID " << (process ? process->nsid() : -1);
};

DarlingServer::Thread::Thread(KernelThreadConstructorTag tag):
	_tid(-1),
	_process(Process::kernelProcess())
{
	static uint64_t kernelThreadIDCounter = DTAPE_KERNEL_THREAD_ID_THRESHOLD;
	static std::mutex kernelThreadIDCounterLock;

	std::unique_lock idLock(kernelThreadIDCounterLock);
	_nstid = kernelThreadIDCounter++;
	if (kernelThreadIDCounter == 0) {
		kernelThreadIDCounter = DTAPE_KERNEL_THREAD_ID_THRESHOLD;
	}
	idLock.unlock();

	_dtapeThread = dtape_thread_create(Process::kernelProcess()->_dtapeTask, _nstid, this);
};

void DarlingServer::Thread::registerWithProcess() {
	std::unique_lock lock(_process->_rwlock);
	_process->_threads[_nstid] = shared_from_this();
};

DarlingServer::Thread::~Thread() noexcept(false) {
	threadLog.info() << *this << ": thread being destroyed" << threadLog.endLog;

	if (_stack.isValid()) {
		stackPool.free(_stack);
	}

	if (!_process) {
		return;
	}

	std::unique_lock lock(_process->_rwlock);
	auto it = _process->_threads.begin();
	while (it != _process->_threads.end()) {
		if (it->first == _nstid) {
			break;
		}
		++it;
	}
	if (it == _process->_threads.end()) {
		throw std::runtime_error("Thread was not registered with Process");
	}
	_process->_threads.erase(it);

	if (_process->_threads.empty()) {
		// if this was the last thread in the process, it has died, so unregister it.
		// this should already be handled by the process' pidfd monitor, but just in case, we also handle it here.
		lock.unlock();
		_process->notifyDead();
	}
};

DarlingServer::Thread::ID DarlingServer::Thread::id() const {
	return _tid;
};

DarlingServer::Thread::NSID DarlingServer::Thread::nsid() const {
	return _nstid;
};

DarlingServer::EternalID DarlingServer::Thread::eternalID() const {
	return _eid;
};

void DarlingServer::Thread::_setEternalID(EternalID eid) {
	_eid = eid;
};

std::shared_ptr<DarlingServer::Process> DarlingServer::Thread::process() const {
	return _process;
};

std::shared_ptr<DarlingServer::Call> DarlingServer::Thread::pendingCall() const {
	std::shared_lock lock(_rwlock);
	return _pendingCall;
};

void DarlingServer::Thread::setPendingCall(std::shared_ptr<Call> newPendingCall) {
	std::unique_lock lock(_rwlock);
	if (newPendingCall && _pendingCall) {
		if (newPendingCall->number() == Call::Number::InterruptEnter) {
			// InterruptEnter calls can occur after we receive a call but before we start processing it,
			// so we need to handle this case gracefully. we do so by saving the interrupt and scheduling
			// it to be processed once the pending call becomes active and suspends or exits.
			_pendingInterrupts.push(newPendingCall);
			return;
		} else {
			throw std::runtime_error("Thread's pending call overwritten while active");
		}
	}
	_pendingCall = newPendingCall;
};

std::shared_ptr<DarlingServer::Call> DarlingServer::Thread::activeCall() const {
	std::shared_lock lock(_rwlock);
	return _activeCall;
};

void DarlingServer::Thread::makePendingCallActive() {
	std::unique_lock lock(_rwlock);
	_activeCall = _pendingCall;
	_pendingCall = nullptr;
};

void DarlingServer::Thread::_deactivateCallLocked(std::shared_ptr<Call> expectedCall) {
	// A0-ARCH stage 3: a call can live in one of three slots -- the plain active slot, or
	// (with an interrupt window open) the frame's interruptedCall / deferred-reply enterCall.
	// Deactivate whichever slot holds it. (The old version keyed on the transient
	// _interruptedForSignal window, which no longer exists.)
	if (_activeCall.get() == expectedCall.get()) {
		_activeCall = nullptr;
		return;
	}
	if (!_interrupts.empty()) {
		if (_interrupts.top().interruptedCall.get() == expectedCall.get()) {
			_interrupts.top().interruptedCall = nullptr;
			return;
		}
		if (_interrupts.top().enterCall.get() == expectedCall.get()) {
			_interrupts.top().enterCall = nullptr;
			return;
		}
	}
	throw std::runtime_error("Upon deactivating the active call found active/interrupted call != expected call");
};

void DarlingServer::Thread::deactivateCall(std::shared_ptr<Call> expectedCall) {
	std::unique_lock lock(_rwlock);
	_deactivateCallLocked(expectedCall);
};

DarlingServer::Address DarlingServer::Thread::address() const {
	std::shared_lock lock(_rwlock);
	return _address;
};

void DarlingServer::Thread::setAddress(Address address) {
	std::unique_lock lock(_rwlock);
	_address = address;
};

void DarlingServer::Thread::setThreadHandles(uintptr_t pthreadHandle, uintptr_t dispatchQueueAddress) {
	dtape_thread_set_handles(_dtapeThread, pthreadHandle, dispatchQueueAddress);
};

bool DarlingServer::Thread::waitingForReply() const {
	std::shared_lock lock(_rwlock);
	return !!_activeCall;
};

/*
 * IMPORTANT
 * ===
 *
 * The way that microthread handling/switching is done here is... not pretty, to say the least.
 * The problem is that, in order to use XNU's Mach IPC code, we need a way to interrupt execution of a "thread" and then resume from the same point.
 * Actual threads are too heavyweight, so instead we use our own form of microthreads with cooperative multitasking.
 * Using actual threads for each managed thread would be much simpler and far less hacky, but far more resource-intensive.
 */

static const auto microthreadLog = DarlingServer::Log("microthread");

// this runs in the context of the microthread (i.e. with the microthread's stack active)
void DarlingServer::Thread::microthreadWorker() {
#if DSERVER_ASAN
	__sanitizer_finish_switch_fiber(asanOldFakeStack, &asanOldStackBottom, &asanOldStackSize);
	asanOldFakeStack = nullptr;
#endif

	currentContinuation = nullptr;
	currentThreadVar->makePendingCallActive();

	// A processCall() implementation may throw (e.g. std::system_error from a memory
	// operation on a guest that died concurrently, or std::runtime_error from the
	// exec-replacement path). This worker runs on a makecontext()-established fiber
	// stack and never returns normally (it always setcontext()s away below), so an
	// uncaught exception unwinding out of here is undefined behavior -> std::terminate
	// -> the whole darlingserver process dies, stranding every other guest with an
	// in-flight RPC blocked forever in recvmsg. Contain it: turn the failure into an
	// error reply when the call supports one, otherwise just log and drop it, and
	// continue the normal post-call flow so the server stays alive.
	// perf #0 (dar-dar6x4-perf-5dq.6): time how long this RPC takes to service and count
	// it. We measure from worker entry (== dequeue, since the worker runs immediately on
	// the same thread that took the item) to completion below.
	auto& _metrics = DarlingServer::Metrics::shared();
	uint64_t _callStartUs = DarlingServer::Metrics::nowMonoUs();
	auto _callNumber = currentThreadVar->_activeCall->number();

	try {
		currentThreadVar->_activeCall->processCall();
	} catch (const std::system_error& err) {
		microthreadLog.error()
			<< "Uncaught std::system_error from processCall (call "
			<< DarlingServer::Call::callNumberToString(currentThreadVar->_activeCall->number())
			<< "): " << err.what() << " (code " << err.code().value() << ")" << microthreadLog.endLog;
		try {
			currentThreadVar->_activeCall->sendBasicReply(-err.code().value());
		} catch (...) {
			// call has no basic reply (returns data); nothing more we can do but survive
		}
	} catch (const std::exception& ex) {
		microthreadLog.error()
			<< "Uncaught exception from processCall (call "
			<< DarlingServer::Call::callNumberToString(currentThreadVar->_activeCall->number())
			<< "): " << ex.what() << microthreadLog.endLog;
		try {
			currentThreadVar->_activeCall->sendBasicReply(-EINVAL);
		} catch (...) {}
	} catch (...) {
		microthreadLog.error()
			<< "Uncaught non-std exception from processCall (call "
			<< DarlingServer::Call::callNumberToString(currentThreadVar->_activeCall->number())
			<< ")" << microthreadLog.endLog;
		try {
			currentThreadVar->_activeCall->sendBasicReply(-EINVAL);
		} catch (...) {}
	}

	// perf #0: record service latency (worker entry -> completion).
	{
		uint64_t now = DarlingServer::Metrics::nowMonoUs();
		uint64_t serviceUs = (now >= _callStartUs) ? (now - _callStartUs) : 0;
		_metrics.rpcsServiced.fetch_add(1, std::memory_order_relaxed);
		_metrics.rpcLatency.record(serviceUs);
		// perf #9 (dar-dar6x4-perf-5dq.16): per-call-number breakdown so perf #7 can
		// see which RPC numbers dominate the guest's recvmsg-wait.
		_metrics.recordCall(static_cast<uint32_t>(_callNumber), serviceUs);
		// perf #18 D9 (dar-1il.4): heatmap. This is the GENERIC fiber worker, so used_fiber=true (the
		// call ran on a suspendable microthread -- a blocking/fiber op, NOT Tier-2-no-fiber-eligible).
		// transport comes from the sticky latch set during dispatch/servicing. perf #18 D14 (dar-1il.9):
		// caller-S2C is no longer a sticky per-thread bool consumed here -- it is attributed per-call at
		// _s2cPerform via Metrics::recordCallerS2cFor(activeCall), so there is no cross-op leak.
		_metrics.recordCallHeatmap(
			static_cast<uint32_t>(_callNumber), serviceUs,
			currentThreadVar->_heatmapCallWasRing ? DarlingServer::Metrics::CallTransport::Ring
			                                      : DarlingServer::Metrics::CallTransport::Uds,
			/* usedFiber */ true,
			/* didCallerS2c */ false);
		currentThreadVar->_heatmapCallWasRing = false;
		if (_callNumber == DarlingServer::Call::Number::Checkin) {
			_metrics.checkinLatency.record(serviceUs);
		}
	}

	// A0-ARCH stage 3: no more _syscallReturnHereDuringInterrupt detour -- every fiber exits
	// through its own doneWorking, on its own stack, exactly once.
#if DSERVER_ASAN
	// we're exiting normally, so we might not re-enter this microthread; tell ASAN to drop the fake stack
	__sanitizer_start_switch_fiber(NULL, asanOldStackBottom, asanOldStackSize);
#endif
	setcontext(&backToThreadTopContext);
	__builtin_unreachable();
};

void DarlingServer::Thread::microthreadContinuation() {
#if DSERVER_ASAN
	__sanitizer_finish_switch_fiber(asanOldFakeStack, &asanOldStackBottom, &asanOldStackSize);
	asanOldFakeStack = nullptr;
#endif

	currentContinuation = currentThreadVar->_continuationCallback;
	// FIXME: we probably should never see `currentContinuation == nullptr`
	if (currentContinuation) {
		currentThreadVar->_continuationCallback = nullptr;
		currentContinuation();
		currentContinuation = nullptr;
	}

#if DSERVER_ASAN
	// see microthreadWorker()
	__sanitizer_start_switch_fiber(NULL, asanOldStackBottom, asanOldStackSize);
#endif
	setcontext(&backToThreadTopContext);
	__builtin_unreachable();
};

void DarlingServer::Thread::doWork() {
	// NOTE: this method MUST NOT use any local variables that require destructors.
	//       this method is actually major UB because the compiler is free to do whatever it likes with the stack,
	//       but we know what reasonable compilers (i.e. GCC and Clang) do with it and we're specifically targeting Clang, so it's okay for us.

	// perf#25a A0 (Part 3c): whether THIS dispatch consumed a genuine resume() wake permit.
	// Only such a dispatch (or a pending-call override) may resume a suspended context: every
	// legitimate waker goes through resume() (thread_unblock finalizes wait_result first), so a
	// dispatch that finds the thread suspended WITHOUT a permit is a stale re-run (e.g. an owed
	// _rerunPending whose wake was already consumed) and must not touch the parked wait --
	// resuming it would deliver a spurious wakeup with wait_result == THREAD_WAITING.
	bool hadResumePermit = false;
	bool preDispatchParked = false;
	bool parkedContextIntact = false;

	// A0-ARCH stage 0 fuzzer: reorder this dispatch behind whatever else is queued (models the
	// dispatch-vs-wake races), and/or inject an extra permit-less dispatch (models stale re-runs).
	// Both are events the protocol MUST tolerate; compiled-in but dormant unless DSERVER_SCHED_FUZZ set.
	if (schedFuzzEnabled) {
		if (schedFuzzChance()) {
			Server::sharedInstance().scheduleThread(shared_from_this());
			return;
		}
		if (schedFuzzChance()) {
			Server::sharedInstance().scheduleThread(shared_from_this());
		}
	}

	_rwlock.lock();

	if (_deferralState != DeferralState::NotDeferred) {
		microthreadLog.debug() << _tid << "(" << _nstid << "): execution was deferred" << microthreadLog.endLog;
		_deferralState = DeferralState::DeferredPending;
		_rwlock.unlock();
		return;
	}

	if (_isRunningLocked()) {
		// perf#25a A0: this dispatch was popped while the microthread is still
		// running on another worker (it is mid suspend()/doneWorking transition;
		// the running view only clears at the doneWorking tail). We cannot run it now,
		// but we MUST NOT silently drop it -- otherwise a wake that a waker
		// delivered via scheduleThread() (rather than via a pending wake token) is lost
		// forever and the microthread deadlocks. Record the owed re-run; the
		// doneWorking tail will reschedule exactly once after _running clears.
		_rerunPending = true;
		_mstateEventLocked(StateEvent::RerunDeferred, "dispatch-while-running");
		microthreadLog.debug() << _tid << "(" << _nstid << "): dispatch arrived while still running; deferring re-run" << microthreadLog.endLog;
		_rwlock.unlock();
		return;
	}

	if (_terminating) {
		goto doneWorking;
	}

	if (_dead && !_activeCall) {
		// should be impossible, since this should be handled in `notifyDead`, but just in case
		_terminating = true;
		goto doneWorking;
	}

	// A0-ARCH stage 2b: whether this dispatch found a parked (resumable) context. Captured
	// BEFORE the transition to Running because the dispatch-decision logic below needs the
	// pre-dispatch view (the state field is authoritative now; there is no lingering
	// _suspended residue to read). Parking is impossible here (the running guard above).
	preDispatchParked = _isSuspendedLocked();
	// A0-ARCH stage 3 (fix 4): the wake-consume moved DOWN into the second locked section,
	// atomically with the resume-vs-stack branch. Consuming here (the old shape, partially
	// gated by fix 1) left a hole: between this section's unlock and the second lock an
	// interrupt_enter can become the pending call, so the dispatch that consumed the permit
	// takes the interrupt-STACKING branch instead of resuming -- the permit is eaten, the
	// nested cancel finds the wait already finalized (fires no replacement wake), and the
	// restored context reparks forever holding a committed grant (captured verbatim on the
	// tape: wake-consumed gen N -> interrupt-push -> repark-interrupt-cancel -> wedge).
	_mstateTransitionLocked(MicroState::Running,
		preDispatchParked ? "dispatch-onto-parked" : "dispatch-fresh");
	currentThreadVar = shared_from_this();
	// A0-ARCH stage 2c: dtape_thread_entering() is GONE, and with it the whole
	// preserveWaitState dance that existed only to keep its unconditional TH_WAIT clobber away
	// from committed waits (the Part 3 history: half-torn waits, zombie children, the brew
	// hang). The dispatch paths never touch the XNU wait state anymore; the wait tear-down
	// paths (thread_unblock / clear_wait_internal / dying) own it, all through the stage-2c
	// write funnel. What remains here is the ASSERTION the old skip-condition encoded: an
	// unambiguous fresh-call dispatch (nothing parked, no override, a fresh non-interrupt
	// call, nothing in flight) must find the guest thread NOT waiting -- a stranded TH_WAIT
	// means some tear-down path failed to clear it (the recovery clear keeps release builds
	// as robust as the old silent launder, but LOUDLY).
	if (!preDispatchParked && !_pendingCallOverride
			&& _pendingCall && _pendingCall->number() != Call::Number::InterruptEnter
			&& !_activeCall && !_continuationCallback
			&& dtape_thread_clear_stranded_wait(_dtapeThread)) {
		_strandedWaitViolationLocked("doWork-fresh-dispatch");
	}

	returningToThreadTop = false;
	_rwlock.unlock();

	_runningCondvar.notify_all();

	getcontext(&backToThreadTopContext);

	if (returningToThreadTop) {
		// someone jumped back to the top of the microthread
		// (that means either the microthread has been suspended or it has finished)

#if DSERVER_ASAN
		__sanitizer_finish_switch_fiber(asanOldFakeStack, &asanOldStackBottom, &asanOldStackSize);
		asanOldFakeStack = nullptr;
#endif

		_rwlock.lock();

		if ((_microState != MicroState::Parking || _continuationCallback) && _stack.isValid()) {
			// we discard the old stack when either:
			//   * we exit normally (i.e. without suspending -- state stayed Running);
			//   * or when we suspend with a continuation callback.
			stackPool.free(_stack);
		}

		//microthreadLog.debug() << _tid << "(" << _nstid << "): microthread returned to top" << microthreadLog.endLog;
		goto doneWorking;
	} else {
		returningToThreadTop = true;

		_rwlock.lock();

		if (!_pendingCallOverride && _pendingCall && _pendingCall->number() == Call::Number::InterruptEnter) {
			// A0-ARCH stage 3: the frame is the PARKING SPOT for the interrupted context while
			// the enter fiber runs -- nothing ever jumps into it from another stack anymore;
			// doneWorking moves it back into the live slots once the enter fiber completes.
			_interrupts.emplace();
			_interrupts.top().savedStack = _stack;
			_interrupts.top().savedResumeContext = _resumeContext;
			_stack = StackPool::Stack();
			_interrupts.top().savedContinuation = _continuationCallback;
			_continuationCallback = nullptr;
			_interrupts.top().interruptedCall = _activeCall;
			_activeCall = nullptr;
			_mstateEventLocked(StateEvent::InterruptPush, "interrupt-enter-stacked", 0, _interrupts.size());
		}

		if (_continuationCallback && _pendingCall) {
			// we can only have one of the two
			throw std::runtime_error("Thread has both a pending call and a pending continuation");
		}

		// A0-ARCH stage 3 (fix 4): consume the resume permit HERE, under the same lock as the
		// branch decision -- a dispatch consumes a wake if and only if it actually resumes the
		// parked context. An interrupt_enter that became pending in the meantime takes the
		// stacking branch above and the wake SURVIVES for the post-restore dispatch.
		if (preDispatchParked && !_pendingCall && _consumePendingWakeLocked()) {
			hadResumePermit = true;
		}

		// perf#25a A0 (Part 3c): resume the suspended context only for a dispatch that carried a
		// real wake (permit consumed above) or an explicit override; a permit-less dispatch of a
		// suspended thread is stale and falls through to the else-branch, which parks it again
		// (no pending call -> doneWorking) instead of spuriously resuming a live wait.
		if (preDispatchParked && (_pendingCallOverride || (!_pendingCall && hadResumePermit))) {
			if (_pendingCallOverride) {
				microthreadLog.info() << _tid << "(" << _nstid << "): thread was suspended with a pending call override and is now resuming with a pending call" << microthreadLog.endLog;
			}
			// we were in the middle of processing a call and we need to resume now
			_resumeContext.uc_link = &backToThreadTopContext;
			_rwlock.unlock();

			if (_continuationCallback) {
				// for continuations, we discard the old stack and start with a new one
				assert(!_stack.isValid());
				stackPool.allocate(_stack);

				// we also ahve to set up the resume context properly with the new stack
				_resumeContext.uc_stack.ss_sp = _stack.base;
				_resumeContext.uc_stack.ss_size = _stack.size;
				_resumeContext.uc_stack.ss_flags = 0;
				_resumeContext.uc_link = &backToThreadTopContext;
				recordMakecontextTrace("continuation", this, _stack, _pendingCall);
				makecontext(&_resumeContext, microthreadContinuation, 0);
			} else {
				// otherwise, we expect to have a valid stack to continue where we left off
				assert(_stack.isValid());
			}

#if DSERVER_ASAN
			__sanitizer_start_switch_fiber(&asanOldFakeStack, _stack.base, _stack.size);
#endif

			setcontext(&_resumeContext);
		} else {
			if (!_pendingCall) {
				// if we don't actually have a pending call, we have nothing to do.
				// A0-ARCH stage 2b: if this no-op dispatch found a parked context, that
				// context was never touched -- doneWorking must REPARK it, not idle the
				// thread (idling here loses the park: the next genuine wake finds
				// "no-park" and is dropped -- captured live as a fuzz boot wedge, the
				// launchd tape ending Parked->Running->Idle then wake-dropped no-park).
				parkedContextIntact = preDispatchParked;
				goto doneWorking;
			}
			_rwlock.unlock();

			// we might've had a valid stack if we're overwriting a previous suspension, so handle that.
			if (_stack.isValid()) {
				stackPool.free(_stack);
			}

			stackPool.allocate(_stack);

			ucontext_t newContext;
			getcontext(&newContext);
			newContext.uc_stack.ss_sp = _stack.base;
			newContext.uc_stack.ss_size = _stack.size;
			newContext.uc_stack.ss_flags = 0;
			newContext.uc_link = &backToThreadTopContext;
			recordMakecontextTrace("worker", this, _stack, _pendingCall);
			makecontext(&newContext, microthreadWorker, 0);

#if DSERVER_ASAN
			__sanitizer_start_switch_fiber(&asanOldFakeStack, _stack.base, _stack.size);
#endif

			setcontext(&newContext);
		}

		// inform the compiler that it shouldn't do anything that would live past the setcontext calls
		__builtin_unreachable();
	}

doneWorking:
	// we must be holding `_rwlock` when we get here
	if (_microState == MicroState::Running || _microState == MicroState::Parking) {
		// A0-ARCH stage 3: if this completed stint was an interrupt_enter whose processCall
		// armed a cancellation, the frame's saved (interrupted) context now moves BACK into
		// the live slots and the thread REPARKS with it -- the machine stays honest (the
		// thread genuinely has a resumable context again), and the pending Xnu wake that
		// sigexc_enter fired makes it Ready -> dispatched normally on its own fiber. The
		// enter fiber's own stack was already freed at return-to-top (its own completion,
		// exactly once); the interruptedCall returns to the active slot for the normal
		// syscall-return/deactivation flow. Only a RUNNING (completed) stint restores: a
		// Parking stint means the enter fiber itself is just suspending mid-flight.
		bool restoredInterrupted = false;
		if (_microState == MicroState::Running && !_interrupts.empty() && _interrupts.top().cancellationArmed) {
			auto& frame = _interrupts.top();
			frame.cancellationArmed = false;
			// the enter call's deferred reply (if owed) lives on in frame.enterCall; the
			// active slot goes back to the interrupted call (null for context-only frames)
			_activeCall = frame.interruptedCall;
			_stack = frame.savedStack;
			frame.savedStack = StackPool::Stack();
			_resumeContext = frame.savedResumeContext;
			_continuationCallback = frame.savedContinuation;
			frame.savedContinuation = nullptr;
			restoredInterrupted = _stack.isValid() || _continuationCallback;
			_mstateEventLocked(StateEvent::Note, "interrupt-cancel-restored", 0, _interrupts.size());
		}
		// Repark when the park was committed THIS stint (Parking), when a no-op
		// dispatch left a previously-parked context untouched (parkedContextIntact --
		// the old `_suspended` flag carried this implicitly; the state field must not
		// lose it or the next wake drops as "no-park" and the thread wedges), or when a
		// cancelled interrupted context was just restored (restoredInterrupted).
		dtape_thread_exiting(_dtapeThread);
		currentThreadVar = nullptr;
		if (_microState == MicroState::Parking || parkedContextIntact || restoredInterrupted) {
			_mstateTransitionLocked(
				_hasDeliverablePendingWakeLocked() ? MicroState::Ready : MicroState::Parked,
				_microState == MicroState::Parking ? "park-committed"
					: (restoredInterrupted ? "repark-interrupt-cancel" : "repark-noop-dispatch"));
		} else {
			_mstateTransitionLocked(MicroState::Idle, "done");
		}
	}
	// A wake can arrive after suspend()'s final permit check but before it
	// physically switches back here. Now that the running view is clear, rescheduling
	// is safe and cannot race another worker running this microthread.
	//
	// perf#25a A0: also honor a dispatch that a worker popped and deferred while
	// we were still running (doWork() set _rerunPending instead of dropping it).
	// Now that the running view is clear it is safe to reschedule; consume the flag so we
	// reschedule exactly once. Only reschedule if the microthread is actually
	// parked with a resumable context and still alive -- if it is
	// terminating/dead or has no context, the owed re-run is moot.
	bool rerunPending = _rerunPending;
	_rerunPending = false;
	// A deliverable pending wake only makes sense when there is a suspended context to
	// resume. _rerunPending is a dropped dispatch (from doWork's running guard): it must be
	// honored whether the microthread is suspended (resume its context) OR has a
	// pending call to process -- i.e. NOT gated on the suspended view. Gate only on liveness.
	bool resumeAfterWorking = ((_hasDeliverablePendingWakeLocked() && _isSuspendedLocked()) || rerunPending) && !_terminating && !_dead;
	bool canRelease = false;
	if (_dead) {
		threadLog.debug() << *this << ": dead thread returning. active call? " << (!!_activeCall ? "true" : "false") << " terminating? " << (_terminating ? "true" : "false") << threadLog.endLog;
	}
	if (_dead && !_activeCall && !_terminating) {
		// this is the case when `notifyDead` notified us we were dead
		// but we had an active call and had to finish it first
		_terminating = true;
		canRelease = true;
		_mstateTransitionLocked(MicroState::Terminated, "dead-after-last-call");
	}
	if (_terminating && !_dead) {
		// this will not destroy our thread immediately;
		// the worker thread invoker still holds a reference on us
		_rwlock.unlock();
		notifyDead();
	} else {
		// if we have any pending interrupts, schedule them to be processed now
		// (we've just finished or suspended a call, so now's the time to handle interrupts)
		if (!_terminating && !_dead && !_pendingInterrupts.empty()) {
			if (_pendingCall) {
				throw std::runtime_error("Need to schedule interrupt for processing, but thread has pending call");
			}

			_pendingCall = _pendingInterrupts.front();
			_pendingInterrupts.pop();

			Server::sharedInstance().scheduleThread(shared_from_this());
		}

		_rwlock.unlock();
	}
	if (unlockMeWhenSuspending) {
		libsimple_lock_unlock(unlockMeWhenSuspending);
		unlockMeWhenSuspending = nullptr;
	}
	_runningCondvar.notify_all();
	if (resumeAfterWorking) {
		Server::sharedInstance().scheduleThread(shared_from_this());
	}
	if (canRelease) {
		_scheduleRelease();
	}
	return;
};

#ifdef DSERVER_RING_TRANSPORT
bool DarlingServer::Thread::doWorkInline() {
	// perf #18 P6.1 (dar-ohp): run a proven-non-blocking Call without the microthread fiber.
	// This is doWork() with the fiber stripped: same guards, same context-enter (so current_task
	// resolves identically), same processCall + exception containment + metrics as
	// microthreadWorker(), then the same essential completion cleanup as doneWorking -- but the
	// call runs on the CURRENT (main-loop) stack instead of a makecontext fiber.
	//
	// SAFETY: the caller guarantees the call never suspends. We therefore never touch the fiber
	// state (backToThreadTopContext / _resumeContext / _stack) and assert it stays non-suspended.
	std::unique_lock<std::shared_mutex> lock(_rwlock);

	if (_deferralState != DeferralState::NotDeferred) {
		_deferralState = DeferralState::DeferredPending;
		return false;
	}
	if (_isRunningLocked()) {
		microthreadLog.warning() << _tid << "(" << _nstid << "): doWorkInline on already-running microthread" << microthreadLog.endLog;
		return false;
	}
	if (_terminating || (_dead && !_activeCall) || _isSuspendedLocked() || _continuationCallback || !_pendingCall) {
		// any of these means this is NOT the simple "fresh non-blocking call" case the inline
		// path is for; let the caller fall back to the full doWork() which handles them.
		return false;
	}

	currentThreadVar = shared_from_this();
	// stage 2c: the guards above proved this a genuinely fresh dispatch (not suspended, no
	// continuation); a stranded TH_WAIT here is the same violation as doWork's
	if (dtape_thread_clear_stranded_wait(_dtapeThread)) {
		_strandedWaitViolationLocked("doWorkInline-dispatch");
	}
	_mstateTransitionLocked(MicroState::Running, "inline-dispatch");
	lock.unlock();
	_runningCondvar.notify_all();

	// --- body: mirror microthreadWorker() without the fiber-return setcontext ---------------
	makePendingCallActive(); // moves _pendingCall -> _activeCall (same as the fiber path)

	auto& _metrics = DarlingServer::Metrics::shared();
	uint64_t _callStartUs = DarlingServer::Metrics::nowMonoUs();
	auto _callNumber = _activeCall->number();

	try {
		_activeCall->processCall();
	} catch (const std::system_error& err) {
		microthreadLog.error() << "Uncaught std::system_error from inline processCall ("
			<< DarlingServer::Call::callNumberToString(_callNumber) << "): " << err.what()
			<< " (code " << err.code().value() << ")" << microthreadLog.endLog;
		try { _activeCall->sendBasicReply(-err.code().value()); } catch (...) {}
	} catch (const std::exception& ex) {
		microthreadLog.error() << "Uncaught exception from inline processCall ("
			<< DarlingServer::Call::callNumberToString(_callNumber) << "): " << ex.what() << microthreadLog.endLog;
		try { _activeCall->sendBasicReply(-EINVAL); } catch (...) {}
	} catch (...) {
		microthreadLog.error() << "Uncaught non-std exception from inline processCall ("
			<< DarlingServer::Call::callNumberToString(_callNumber) << ")" << microthreadLog.endLog;
		try { _activeCall->sendBasicReply(-EINVAL); } catch (...) {}
	}

	{
		uint64_t now = DarlingServer::Metrics::nowMonoUs();
		uint64_t serviceUs = (now >= _callStartUs) ? (now - _callStartUs) : 0;
		_metrics.rpcsServiced.fetch_add(1, std::memory_order_relaxed);
		_metrics.rpcLatency.record(serviceUs);
		_metrics.recordCall(static_cast<uint32_t>(_callNumber), serviceUs);
		// perf #18 D9 (dar-1il.4): heatmap. This is doWorkInline -- the Tier-2 NO-FIBER ring path. By
		// construction it ran a ring-originated allowlisted op without suspending (used_fiber=false); a
		// real suspend here is a contract violation handled below. transport=ring. perf #18 D14
		// (dar-1il.9): caller-S2C attributed per-call at _s2cPerform, not via a sticky latch here.
		_metrics.recordCallHeatmap(
			static_cast<uint32_t>(_callNumber), serviceUs,
			DarlingServer::Metrics::CallTransport::Ring,
			/* usedFiber */ false,
			/* didCallerS2c */ false);
		_heatmapCallWasRing = false;
	}

	// processCall() on a non-blocking op must have run to completion. If somehow it suspended
	// (a misclassified op), that is a contract violation -> we'd have corrupted the fiber model.
	// Detect it loudly rather than silently mishandle.
	bool canRelease = false;
	{
		std::unique_lock<std::shared_mutex> relock(_rwlock);
		if (_microState == MicroState::Parking) {
			// Should be impossible for an allowlisted op. The microthread "suspended" without a
			// fiber to resume onto -> we cannot honor it. Log; the Idle transition below
			// recovers (the one legal Parking->Idle edge) so the thread isn't wedged.
			// (The guest will time out on this op and UDS-fall-back.)
			microthreadLog.error() << *this << ": doWorkInline call suspended -- not fast-path eligible!" << microthreadLog.endLog;
			DarlingServer::Metrics::shared().ringFastSuspend.fetch_add(1, std::memory_order_relaxed);
		}
		_activeCall = nullptr;
		dtape_thread_exiting(_dtapeThread);
		currentThreadVar = nullptr;
		_mstateTransitionLocked(MicroState::Idle, "inline-done");

		if (_dead && !_activeCall && !_terminating) {
			_terminating = true;
			canRelease = true;
			_mstateTransitionLocked(MicroState::Terminated, "dead-after-last-call");
		}
		if (_terminating && !_dead) {
			relock.unlock();
			notifyDead();
		} else {
			if (!_terminating && !_dead && !_pendingInterrupts.empty()) {
				if (!_pendingCall) {
					_pendingCall = _pendingInterrupts.front();
					_pendingInterrupts.pop();
					Server::sharedInstance().scheduleThread(shared_from_this());
				}
			}
			relock.unlock();
		}
	}
	_runningCondvar.notify_all();
	if (canRelease) {
		_scheduleRelease();
	}
	return true;
};

bool DarlingServer::Thread::doMachReplyPortInline(uint32_t seq) {
	// perf #18 P6.1 step 2 (dar-ohp): the surgical one-op path. This is doWorkInline() with the
	// Call/Message framing ALSO removed: no _pendingCall, no processCall(), no callFromMessage --
	// we run the bare dtape primitive and publish the reply onto the ring ourselves. The duct-tape
	// context setup/teardown is IDENTICAL to doWorkInline (so current_task() resolves to this
	// thread's space exactly as MachReplyPort::processCall would see it).
	auto ring = _ring;
	if (!ring) {
		return false; // caller falls back to the generic path
	}

	std::unique_lock<std::shared_mutex> lock(_rwlock);

	if (_deferralState != DeferralState::NotDeferred) {
		_deferralState = DeferralState::DeferredPending;
		return false;
	}
	if (_isRunningLocked()) {
		microthreadLog.warning() << _tid << "(" << _nstid << "): doMachReplyPortInline on already-running microthread" << microthreadLog.endLog;
		return false;
	}
	if (_terminating || _dead || _isSuspendedLocked() || _continuationCallback || _pendingCall || _activeCall) {
		// Not the simple "fresh, idle thread servicing a no-arg trap" case. There is no Call to run
		// here (we bypass callFromMessage), so a _pendingCall/_activeCall would be left dangling --
		// decline and let the caller take the generic step-1 path which handles all of these.
		return false;
	}

	currentThreadVar = shared_from_this();
	if (dtape_thread_clear_stranded_wait(_dtapeThread)) {
		_strandedWaitViolationLocked("doMachReplyPortInline-dispatch");
	}
	_mstateTransitionLocked(MicroState::Running, "inline-mrp-dispatch");
	lock.unlock();
	_runningCondvar.notify_all();

	// --- body: the bare Mach primitive, identical to what MachReplyPort::processCall calls ------
	auto& _metrics = DarlingServer::Metrics::shared();
	uint64_t _callStartUs = DarlingServer::Metrics::nowMonoUs();
	uint32_t port = dtape_mach_reply_port();

	// publish {replyhdr.code=0}{uint32 port_name} straight onto the s2c ring + wake the guest.
	// Byte-identical to the reply the generic path produces via pushCallReply for this op.
#ifdef DSERVER_RING_PHASE_PROF
	uint64_t _pubT0 = Metrics::rdtscCycles();
#endif
	bool published = ring->publishReply(seq, static_cast<uint32_t>(dserver_callnum_mach_reply_port), 0, &port, sizeof(port));
#ifdef DSERVER_RING_PHASE_PROF
	_ringPublishCycles = Metrics::rdtscCycles() - _pubT0;
#endif
	if (published) {
		ring->wakeGuest();
	} else {
		// s2c ring full: the reply will be sent via UDS below (no double-mint). Account it so a
		// nonzero ring_s2c_full under load flags a guest that isn't draining its s2c ring.
		Metrics::shared().ringS2cFull.fetch_add(1, std::memory_order_relaxed);
	}

	{
		uint64_t now = DarlingServer::Metrics::nowMonoUs();
		uint64_t serviceUs = (now >= _callStartUs) ? (now - _callStartUs) : 0;
		_metrics.rpcsServiced.fetch_add(1, std::memory_order_relaxed);
		_metrics.rpcLatency.record(serviceUs);
		_metrics.recordCall(static_cast<uint32_t>(dserver_callnum_mach_reply_port), serviceUs);
		// perf #18 D9 (dar-1il.4): heatmap. The surgical mach_reply_port path: ring transport, no fiber,
		// pure mint (never an S2C). It bypasses beginRingReply/processCall, so pass the facts directly.
		_metrics.recordCallHeatmap(
			static_cast<uint32_t>(dserver_callnum_mach_reply_port), serviceUs,
			DarlingServer::Metrics::CallTransport::Ring,
			/* usedFiber */ false, /* didCallerS2c */ false);
	}

	// --- completion cleanup: mirror doWorkInline's (no fiber, no _activeCall ever set) ----------
	bool canRelease = false;
	{
		std::unique_lock<std::shared_mutex> relock(_rwlock);
		if (_microState == MicroState::Parking) {
			// Impossible for mach_reply_port (it never blocks). Log loudly; the Idle
			// transition below recovers so we don't wedge.
			microthreadLog.error() << *this << ": doMachReplyPortInline suspended -- mach_reply_port must never block!" << microthreadLog.endLog;
			DarlingServer::Metrics::shared().ringFastSuspend.fetch_add(1, std::memory_order_relaxed);
		}
		dtape_thread_exiting(_dtapeThread);
		currentThreadVar = nullptr;
		_mstateTransitionLocked(MicroState::Idle, "inline-mrp-done");

		if (_dead && !_activeCall && !_terminating) {
			_terminating = true;
			canRelease = true;
			_mstateTransitionLocked(MicroState::Terminated, "dead-after-last-call");
		}
		if (_terminating && !_dead) {
			relock.unlock();
			notifyDead();
		} else {
			if (!_terminating && !_dead && !_pendingInterrupts.empty()) {
				if (!_pendingCall) {
					_pendingCall = _pendingInterrupts.front();
					_pendingInterrupts.pop();
					Server::sharedInstance().scheduleThread(shared_from_this());
				}
			}
			relock.unlock();
		}
	}
	_runningCondvar.notify_all();
	if (canRelease) {
		_scheduleRelease();
	}

	// If the publish failed (s2c full), tell the caller to fall back so the guest still gets a
	// reply via the generic path. The dtape trap already ran (it minted a real port); re-running
	// it via callFromMessage would leak that port. So instead of "return false to re-dispatch",
	// we treat a publish failure as a HARD inline failure only when nothing was minted. Here the
	// port WAS minted and the only loss is the wake; publishReply already UDS-falls-back inside
	// pushCallReply for the generic path, but we don't have that here. Simplest correct choice:
	// if publish failed, send the reply via UDS directly so we never double-mint.
	if (!published) {
		// Build the minimal UDS reply and send it. Reuse the same reply convention.
		dserver_rpc_reply_mach_reply_port_t reply;
		reply.header.number = dserver_callnum_mach_reply_port;
		reply.header.code = 0;
		reply.body.port_name = port;
		Message replyMsg(sizeof(reply), 0);
		replyMsg.data().resize(sizeof(reply));
		memcpy(replyMsg.data().data(), &reply, sizeof(reply));
		replyMsg.setAddress(_address);
		Server::sharedInstance().sendMessage(std::move(replyMsg));
	}
	return true;
};
#endif

void DarlingServer::Thread::suspend(std::function<void()> continuationCallback, libsimple_lock_t* unlockMe) {
	if (this != currentThreadVar.get()) {
		throw std::runtime_error("Attempt to suspend thread other than current thread");
	}

	if (interruptDisableCount > 0) {
		throw std::runtime_error("Attempt to suspend thread while interrupts disabled");
	}

	_rwlock.lock();
	// Consume a matching wake that arrived before suspend() marked us suspended.
	if (_consumeWakeBeforeSuspendLocked()) {
		_rwlock.unlock();
		if (unlockMe) {
			libsimple_lock_unlock(unlockMe);
		}
		return;
	}
	{
		// tape the armed-kind mask so a stuck park names what it is waiting for
		uint64_t armedMask = 0;
		for (size_t kindIndex = 0; kindIndex < wakeKindCount; kindIndex++) {
			if (_armedWakeGen[kindIndex] != 0) {
				armedMask |= (1ull << kindIndex);
			}
		}
		_mstateTransitionLocked(MicroState::Parking, "park", 0, armedMask);
	}
	_rwlock.unlock();

	unlockMeWhenSuspending = unlockMe;

	getcontext(&_resumeContext);

	_rwlock.lock();
	// Consume a matching wake that arrived while the resume context was being captured.
	if (_consumeWakeAfterContextCaptureLocked()) {
		_rwlock.unlock();
		if (unlockMeWhenSuspending) {
			libsimple_lock_unlock(unlockMeWhenSuspending);
			unlockMeWhenSuspending = nullptr;
		}
		return;
	}
	if (_microState == MicroState::Parking) {
		if (continuationCallback) {
			// when suspendeding with a continuation, the current continuation and call are discarded (since they can no longer be safely returned to)
			currentContinuation = nullptr;

			_continuationCallback = continuationCallback;
		}
		// jump back to the top of the microthread
		_rwlock.unlock();

#if DSERVER_ASAN
		// if we have a continuation, we don't expect to come back here
		__sanitizer_start_switch_fiber((continuationCallback) ? nullptr : &asanOldFakeStack, asanOldStackBottom, asanOldStackSize);
#endif

		setcontext(&backToThreadTopContext);
		__builtin_unreachable();
	} else {
		// we've been resumed

		// make sure we don't have a continuation when we get here;
		// if we do, that means that doWork() failed to do its job for the continuation case
		assert(!_continuationCallback);

		_rwlock.unlock();

#if DSERVER_ASAN
		__sanitizer_finish_switch_fiber(asanOldFakeStack, &asanOldStackBottom, &asanOldStackSize);
		asanOldFakeStack = nullptr;
#endif
	}
};

// A0-ARCH stage 2a: shadow run-state machine. Violations are log-and-survive by default
// so a mismodeled edge cannot take down a production server; DSERVER_MSTATE_ABORT=1
// (fuzz/gate legs) makes them fatal so the fuzzer can catch them as crashes.
static bool mstateAbortOnViolation = false;
static bool mstateAbortInit() {
	const char* value = getenv("DSERVER_MSTATE_ABORT");
	return value && value[0] && value[0] != '0';
};
static const bool mstateAbortInitDone = (mstateAbortOnViolation = mstateAbortInit(), true);

bool DarlingServer::Thread::_isRunningLocked() const {
	return _microState == MicroState::Running || _microState == MicroState::Parking || _impersonationPin;
};

bool DarlingServer::Thread::_isSuspendedLocked() const {
	return _microState == MicroState::Parking || _microState == MicroState::Parked || _microState == MicroState::Ready;
};

const char* DarlingServer::Thread::microStateName(MicroState state) {
	switch (state) {
		case MicroState::Idle:       return "Idle";
		case MicroState::Running:    return "Running";
		case MicroState::Parking:    return "Parking";
		case MicroState::Parked:     return "Parked";
		case MicroState::Ready:      return "Ready";
		case MicroState::Terminated: return "Terminated";
	}
	return "?";
};

const char* DarlingServer::Thread::stateEventName(StateEvent event) {
	switch (event) {
		case StateEvent::Transition:       return "transition";
		case StateEvent::ArmWake:          return "arm";
		case StateEvent::DisarmWake:       return "disarm";
		case StateEvent::WakePending:      return "wake-pending";
		case StateEvent::WakeDropped:      return "wake-dropped";
		case StateEvent::WakeConsumed:     return "wake-consumed";
		case StateEvent::InterruptPush:    return "interrupt-push";
		case StateEvent::InterruptPop:     return "interrupt-pop";
		case StateEvent::RerunDeferred:    return "rerun-deferred";
		case StateEvent::ImpersonatePin:   return "impersonate-pin";
		case StateEvent::ImpersonateUnpin: return "impersonate-unpin";
		case StateEvent::Note:             return "note";
		case StateEvent::XnuWait:          return "xwait";
	}
	return "?";
};

static bool mstateLegalTransition(DarlingServer::Thread::MicroState from, DarlingServer::Thread::MicroState to) {
	using MS = DarlingServer::Thread::MicroState;
	switch (from) {
		case MS::Idle:
			// Running = fresh dispatch; Parked = kernel-thread birth park;
			// Terminated = died with nothing left to run
			return to == MS::Running || to == MS::Parked || to == MS::Terminated;
		case MS::Running:
			// Running->Parked is the no-op-dispatch repark: a stale/permit-less dispatch of a
			// Parked thread takes ownership (Parked->Running "dispatch-stale"), never touches
			// the parked context (no pending call, no consumable wake), and doneWorking reparks
			// it directly -- the park was never un-committed, so suspend()/Parking is not
			// re-traversed. (First 2a fuzz finding: every fuzz leg tripped exactly this edge.)
			// Running->Ready is the same repark with a wake that landed DURING the no-op
			// dispatch window (wake() only records a pending while the state is Running;
			// doneWorking's tail then resolves the repark to Ready instead of Parked).
			return to == MS::Parking || to == MS::Parked || to == MS::Ready || to == MS::Idle || to == MS::Terminated;
		case MS::Parking:
			// Parked/Ready = doneWorking committed the park; Running = a wake raced the park
			// (suspend()'s post-getcontext check); Idle = inline-path suspend contract
			// violation recovery (that site logs loudly on its own)
			return to == MS::Parked || to == MS::Ready || to == MS::Running || to == MS::Idle;
		case MS::Parked:
			// Running covers both a consuming resume and a stale/fresh-call dispatch
			return to == MS::Running || to == MS::Ready || to == MS::Terminated;
		case MS::Ready:
			return to == MS::Running || to == MS::Terminated;
		case MS::Terminated:
			return false;
	}
	return false;
};

void DarlingServer::Thread::_mstateRecordLocked(StateEvent event, MicroState from, MicroState to, const char* reason, uint8_t aux8, uint64_t aux64) {
	StateTapeEntry& entry = _stateTape[_stateTapeCount % stateTapeCapacity];
	_stateTapeCount++;
	entry.timeUs = DarlingServer::Metrics::nowMonoUs();
	entry.aux64 = aux64;
	entry.reason = reason;
	entry.event = event;
	entry.from = from;
	entry.to = to;
	entry.aux8 = aux8;
};

void DarlingServer::Thread::_mstateEventLocked(StateEvent event, const char* reason, uint8_t aux8, uint64_t aux64) {
	_mstateRecordLocked(event, _microState, _microState, reason, aux8, aux64);
};

void DarlingServer::Thread::_mstateTransitionLocked(MicroState to, const char* reason, uint8_t aux8, uint64_t aux64) {
	MicroState from = _microState;
	_microState = to;
	_mstateRecordLocked(StateEvent::Transition, from, to, reason, aux8, aux64);

	// legality assertion. (Stage 2b flipped authority: the old _running/_suspended flags
	// are now derived views of this field, so the 2a flag-consistency checks are
	// tautological and gone; the from->to table is the contract.)
	const char* violation = nullptr;
	if (!mstateLegalTransition(from, to)) {
		violation = "illegal transition";
	}
	if (violation) {
		microthreadLog.error() << _tid << "(" << _nstid << "): MSTATE VIOLATION: " << violation
			<< " (" << microStateName(from) << " -> " << microStateName(to) << ", " << reason << ")" << microthreadLog.endLog;
		_dumpStateTapeLocked(violation);
		if (mstateAbortOnViolation) {
			abort();
		}
	}
};

void DarlingServer::Thread::_dumpStateTapeLocked(const char* why) const {
	microthreadLog.error() << _tid << "(" << _nstid << "): state tape dump (" << why << "): state="
		<< microStateName(_microState) << ", " << _stateTapeCount << " events total" << microthreadLog.endLog;
	uint64_t count = (_stateTapeCount < stateTapeCapacity) ? _stateTapeCount : stateTapeCapacity;
	for (uint64_t i = _stateTapeCount - count; i < _stateTapeCount; i++) {
		const StateTapeEntry& entry = _stateTape[i % stateTapeCapacity];
		microthreadLog.error() << _tid << ": tape[" << i << "] t=" << entry.timeUs << "us "
			<< stateEventName(entry.event) << " " << microStateName(entry.from) << "->" << microStateName(entry.to)
			<< " reason=" << (entry.reason ? entry.reason : "-")
			<< " aux8=" << (int)entry.aux8 << " aux64=" << entry.aux64 << microthreadLog.endLog;
	}
};

void DarlingServer::Thread::dumpCurrentThreadStateTape() {
	// panic funnel: no locks (the process is dying and may hold them), print like panic()
	Thread* self = currentThreadVar.get();
	if (!self) {
		printf("mstate: no current microthread at panic\n");
		fflush(stdout);
		return;
	}
	printf("mstate: tid=%d nstid=%d state=%s events=%llu\n", self->_tid, self->_nstid,
		microStateName(self->_microState), (unsigned long long)self->_stateTapeCount);
	uint64_t total = self->_stateTapeCount;
	uint64_t count = (total < stateTapeCapacity) ? total : stateTapeCapacity;
	for (uint64_t i = total - count; i < total; i++) {
		const StateTapeEntry& entry = self->_stateTape[i % stateTapeCapacity];
		printf("mstate: tape[%llu] t=%lluus %s %s->%s reason=%s aux8=%u aux64=%llu\n",
			(unsigned long long)i, (unsigned long long)entry.timeUs,
			stateEventName(entry.event), microStateName(entry.from), microStateName(entry.to),
			entry.reason ? entry.reason : "-", (unsigned)entry.aux8, (unsigned long long)entry.aux64);
	}
	fflush(stdout);
};

void DarlingServer::Thread::recordXnuWaitTransition(const char* reason, uint32_t oldState, uint32_t newState, int32_t waitResult, uint8_t flags) {
	std::unique_lock lock(_rwlock);
	_mstateEventLocked(StateEvent::XnuWait, reason, (uint8_t)(newState & 0xff),
		((uint64_t)(oldState & 0xff) << 32) | (uint64_t)(uint32_t)waitResult);
	if (flags & DTAPE_XWAIT_VIOLATION) {
		microthreadLog.error() << _tid << "(" << _nstid << "): MSTATE VIOLATION (xwait): " << reason
			<< " (state 0x" << std::hex << oldState << " -> 0x" << newState << std::dec
			<< ", wait_result " << waitResult << ")" << microthreadLog.endLog;
		_dumpStateTapeLocked(reason);
		if (mstateAbortOnViolation) {
			abort();
		}
	}
};

void DarlingServer::Thread::_strandedWaitViolationLocked(const char* site) {
	_mstateEventLocked(StateEvent::XnuWait, site, 0, 0);
	microthreadLog.error() << _tid << "(" << _nstid << "): MSTATE VIOLATION (xwait): fresh dispatch found"
		<< " stranded TH_WAIT (recovery-cleared) at " << site << microthreadLog.endLog;
	_dumpStateTapeLocked(site);
	if (mstateAbortOnViolation) {
		abort();
	}
};

uint64_t DarlingServer::Thread::armWake(WakeKind kind) {
	std::unique_lock lock(_rwlock);
	uint64_t gen = ++_wakeGenCounter;
	const size_t k = static_cast<size_t>(kind);
	if (_armedWakeGen[k] != 0) {
		// the previous wait of this kind ended without its arm-site disarm running (its wake
		// was consumed elsewhere, or the park was aborted); re-arming supersedes it
		microthreadLog.info() << _tid << "(" << _nstid << "): re-arming wake kind " << (int)k
			<< " over live gen " << _armedWakeGen[k] << microthreadLog.endLog;
	}
	_armedWakeGen[k] = gen;
	if (_pendingWakeGen[k] != 0) {
		// any wake still pending for this kind was for a PREVIOUS wait -- stale by
		// construction (gen is fresh); drop it so it cannot satisfy the new wait
		microthreadLog.info() << _tid << "(" << _nstid << "): dropping stale pending wake kind "
			<< (int)k << " gen " << _pendingWakeGen[k] << " at re-arm" << microthreadLog.endLog;
		_mstateEventLocked(StateEvent::WakeDropped, "stale-at-re-arm", (uint8_t)k, _pendingWakeGen[k]);
		_pendingWakeGen[k] = 0;
	}
	_mstateEventLocked(StateEvent::ArmWake, "arm", (uint8_t)k, gen);
	return gen;
};

void DarlingServer::Thread::disarmWake(WakeKind kind) {
	std::unique_lock lock(_rwlock);
	const size_t k = static_cast<size_t>(kind);
	_mstateEventLocked(StateEvent::DisarmWake, "disarm", (uint8_t)k, _armedWakeGen[k]);
	_armedWakeGen[k] = 0;
	// old Part 3d lives here now: a wake already delivered for this (concluded) wait is
	// satisfied by definition -- drop it so it cannot go stale and spuriously satisfy the
	// thread's NEXT suspend() while wait_result is still THREAD_WAITING.
	_pendingWakeGen[k] = 0;
};

bool DarlingServer::Thread::_consumePendingWakeLocked() {
	// Abort outranks everything and matches unconditionally (doWork routes a resumed dying
	// thread into its terminating paths).
	constexpr size_t abortIdx = static_cast<size_t>(WakeKind::Abort);
	if (_pendingWakeGen[abortIdx] != 0) {
		_pendingWakeGen[abortIdx] = 0;
		_mstateEventLocked(StateEvent::WakeConsumed, "consume-abort", (uint8_t)abortIdx, 1);
		return true;
	}
	// Prefer the innermost park kinds: a raw queue handoff belongs to the park physically on
	// the stack right now; an XNU-wait finalization may belong to an OUTER wait (nested
	// arming) whose early pop the loop-guarded parks tolerate by re-checking.
	static constexpr WakeKind order[] = { WakeKind::Raw, WakeKind::Xnu, WakeKind::UserSuspension, WakeKind::Kick };
	for (WakeKind kind : order) {
		const size_t k = static_cast<size_t>(kind);
		if (_pendingWakeGen[k] == 0) {
			continue;
		}
		if (_pendingWakeGen[k] == _armedWakeGen[k]) {
			// consuming clears BOTH slots: the wait is being delivered right now, so the
			// arm-site's own disarm (which would drop a still-pending wake) becomes a no-op
			_mstateEventLocked(StateEvent::WakeConsumed, "consume", (uint8_t)k, _pendingWakeGen[k]);
			_pendingWakeGen[k] = 0;
			_armedWakeGen[k] = 0;
			return true;
		}
		// pending no longer matches the armed wait of its kind -- its wait is gone; drop
		microthreadLog.info() << _tid << "(" << _nstid << "): dropping stale pending wake kind "
			<< (int)k << " gen " << _pendingWakeGen[k] << " (armed " << _armedWakeGen[k] << ")" << microthreadLog.endLog;
		_mstateEventLocked(StateEvent::WakeDropped, "stale-at-consume", (uint8_t)k, _pendingWakeGen[k]);
		_pendingWakeGen[k] = 0;
	}
	return false;
};

bool DarlingServer::Thread::_consumeWakeBeforeSuspendLocked() {
	if (!_consumePendingWakeLocked()) {
		return false;
	}
	TestDiagnostics::traceLine("microthread.suspend.consume_pending_resume");
	return true;
};

bool DarlingServer::Thread::_consumeWakeAfterContextCaptureLocked() {
	if (!_consumePendingWakeLocked()) {
		return false;
	}
	TestDiagnostics::traceLine("microthread.suspend.consume_resume_during_suspend");
	if (_microState == MicroState::Parking) {
		_mstateTransitionLocked(MicroState::Running, "wake-raced-park");
	}
	return true;
};

bool DarlingServer::Thread::_hasDeliverablePendingWakeLocked() const {
	if (_pendingWakeGen[static_cast<size_t>(WakeKind::Abort)] != 0) {
		return true;
	}
	for (size_t k = 0; k < wakeKindCount; k++) {
		if (_pendingWakeGen[k] != 0 && _pendingWakeGen[k] == _armedWakeGen[k]) {
			return true;
		}
	}
	return false;
};

void DarlingServer::Thread::wake(WakeKind kind, uint64_t generation) {
	bool schedule = false;
	{
		std::unique_lock lock(_rwlock);
		if (!_isRunningLocked() && !_isSuspendedLocked()) {
			// nothing to deliver to (no parked context and no owner mid-transition);
			// same no-op as the old untyped resume()
			_mstateEventLocked(StateEvent::WakeDropped, "no-park", (uint8_t)kind, generation);
			return;
		}
		if (kind == WakeKind::Abort) {
			_pendingWakeGen[static_cast<size_t>(WakeKind::Abort)] = 1;
			_mstateEventLocked(StateEvent::WakePending, "abort", (uint8_t)WakeKind::Abort, 1);
		} else {
			size_t k = static_cast<size_t>(kind);
			if (generation == 0) {
				// waker cannot know the generation (thread_release; kernel-thread birth
				// unblock): target the currently-armed wait of this kind
				generation = _armedWakeGen[k];
				if (generation == 0 && kind == WakeKind::Xnu) {
					// kernel_thread_create sets TH_WAIT directly without
					// thread_mark_wait_locked, so the thread's FIRST unblock arrives
					// untyped -- deliver it as the startup kick
					kind = WakeKind::Kick;
					k = static_cast<size_t>(WakeKind::Kick);
					generation = _armedWakeGen[k];
				}
				if (generation == 0) {
					microthreadLog.info() << _tid << "(" << _nstid << "): dropping untyped wake kind "
						<< (int)k << ": nothing armed" << microthreadLog.endLog;
					_mstateEventLocked(StateEvent::WakeDropped, "untyped-nothing-armed", (uint8_t)k, 0);
					return;
				}
			} else if (_armedWakeGen[k] != generation) {
				// STALE WAKE: the wait this wake was minted for is gone (consumed early by
				// an abort, superseded by a re-arm). This is the A0 crosstalk class -- the
				// old untyped permit would have delivered it as a spurious resume; drop it.
				microthreadLog.info() << _tid << "(" << _nstid << "): dropping stale wake kind "
					<< (int)k << " gen " << generation << " (armed " << _armedWakeGen[k] << ")" << microthreadLog.endLog;
				_mstateEventLocked(StateEvent::WakeDropped, "stale-wake", (uint8_t)k, generation);
				return;
			}
			_pendingWakeGen[k] = generation;
			_mstateEventLocked(StateEvent::WakePending, "wake", (uint8_t)k, generation);
		}
		if (_microState == MicroState::Parked && _hasDeliverablePendingWakeLocked()) {
			// the park is committed and this wake matches it: the thread now owes a dispatch
			_mstateTransitionLocked(MicroState::Ready, "wake-deliverable", (uint8_t)kind, generation);
		}
		schedule = _isSuspendedLocked() && !_isRunningLocked();
	}

	// A0-ARCH stage 0 fuzzer: occasionally force a dispatch even though the thread is still
	// running / not yet parked -- models the wake-while-running reorder (_rerunPending path).
	if (schedFuzzEnabled && !schedule && schedFuzzChance()) {
		schedule = true;
	}

	if (schedule) {
		Server::sharedInstance().scheduleThread(shared_from_this());
	}
};

void DarlingServer::Thread::terminate() {
	if (_process) {
		if (_process.get() != Process::kernelProcess().get()) {
			throw std::runtime_error("terminate() called on non-kernel thread");
		}
	} else {
		throw std::runtime_error("terminate() called on non-kernel thread");
	}

	_rwlock.lock();
	_terminating = true;
	_mstateEventLocked(StateEvent::Note, "terminate");

	if (currentThreadVar.get() == this) {
		// if it's the current thread, just suspend it;
		// when we return to the "top" of the microthread,
		// doWork() will see that it's terminating and clean up
		_rwlock.unlock();
		suspend();
		throw std::runtime_error("terminate() on current kernel thread returned");
	} else {
		// if it's not the current thread and it's not currently running, just tell it died;
		// it should die once the caller releases their reference(s) on us
		if (!_isRunningLocked()) {
			_rwlock.unlock();
			notifyDead();
		} else {
			// otherwise, if it IS running, once it returns to the microthread "top" and sees `_terminating = true`, it'll unregister itself
			_rwlock.unlock();
		}
	}
};

std::shared_ptr<DarlingServer::Thread> DarlingServer::Thread::currentThread() {
	return currentThreadVar;
};

void DarlingServer::Thread::setupKernelThread(std::function<void()> startupCallback) {
	std::unique_lock lock(_rwlock);
	_continuationCallback = startupCallback;
	// A0-ARCH stage 1: the birth park of a kernel thread; woken exactly once by a startup
	// kick (startKernelThread below, or an untyped first thread_unblock for kernel threads
	// created via kernel_thread_create -- see wake()).
	_armedWakeGen[static_cast<size_t>(WakeKind::Kick)] = ++_wakeGenCounter;
	_mstateEventLocked(StateEvent::ArmWake, "kthread-birth-kick", (uint8_t)WakeKind::Kick, _armedWakeGen[static_cast<size_t>(WakeKind::Kick)]);
	_mstateTransitionLocked(MicroState::Parked, "kthread-birth");
	getcontext(&_resumeContext);
};

void DarlingServer::Thread::startKernelThread(std::function<void()> startupCallback) {
	setupKernelThread(startupCallback);
	wake(WakeKind::Kick, 0);
};

void DarlingServer::Thread::impersonate(std::shared_ptr<Thread> thread) {
	std::shared_ptr<Thread> oldThread;

	if (thread) {
		// prevent the thread from running while we're impersonating it
		// FIXME: this may lead to blocking natively while we're on a microthread.
		//        we would prefer to block using duct-taped facilities instead.
		{
			std::unique_lock lock(thread->_rwlock);
			thread->_deferLocked(true, lock);
			// A0-ARCH stage 2b: the lockout is its own field now (it used to be smuggled
			// through `_running = true` on a non-running thread); _isRunningLocked folds
			// it in so defer/waitUntil* readers see the same "busy" they always did
			thread->_impersonationPin = true;
			thread->_mstateEventLocked(StateEvent::ImpersonatePin, "impersonate");
		}
		thread->_runningCondvar.notify_all();
	}

	{
		std::unique_lock lock(_rwlock);
		oldThread = _impersonating;
		_impersonating = thread;
	}

	if (oldThread) {
		{
			std::unique_lock lock(oldThread->_rwlock);
			oldThread->_impersonationPin = false;
			oldThread->_mstateEventLocked(StateEvent::ImpersonateUnpin, "impersonate-end");
			oldThread->_undeferLocked(lock);
		}
		oldThread->_runningCondvar.notify_all();
	}
};

std::shared_ptr<DarlingServer::Thread> DarlingServer::Thread::impersonatingThread() const {
	std::shared_lock lock(_rwlock);
	return _impersonating;
};

void DarlingServer::Thread::interruptDisable() {
	++interruptDisableCount;
};

void DarlingServer::Thread::interruptEnable() {
	if (interruptDisableCount-- == 0) {
		throw std::runtime_error("interruptEnable() called when already enabled");
	}
};

void DarlingServer::Thread::syscallReturn(int resultCode) {
	if (!currentThreadVar) {
		throw std::runtime_error("syscallReturn() called with no current thread");
	}

	{
		// A0-ARCH stage 3: a cancelled interrupted call runs here via the NORMAL dispatch
		// path, with itself restored as the active call -- no special interrupt-slot lookup,
		// no setcontext detour. Its reply lands in pushCallReply, which stashes it on the
		// open interrupt frame and sends the owed interrupt_enter reply.
		auto call = currentThreadVar->_activeCall;
		if (!call || !call->isXNUTrap()) {
			throw std::runtime_error("Attempt to return from syscall on thread with no active syscall");
		}
		if (call->isBSDTrap()) {
			call->sendBSDReply(resultCode, currentThreadVar->_bsdReturnValue);
		} else {
			call->sendBasicReply(resultCode);
		}
	}

	// jump back to the top of the thread
#if DSERVER_ASAN
	__sanitizer_start_switch_fiber(nullptr, asanOldStackBottom, asanOldStackSize);
#endif
	setcontext(&backToThreadTopContext);
	__builtin_unreachable();
};

static std::queue<std::function<void()>> kernelAsyncRunnerQueue;

// we have to use a regular lock here because it needs to be lockable from both a microthread and normal thread context.
// additionally, it's only locked for brief periods.
//
// we use libsimple_lock_t so we can pass it to `suspend` to unlock it after suspending.
// XXX: we could use a std::mutex if we add an overload to `suspend` for it.
static libsimple_lock_t kernelAsyncRunnerQueueLock;
static dtape_semaphore_t* kernelAsyncRunnerQueueSempahore = nullptr;
static uint64_t kernelAsyncRunnersAvailable = 0;
static std::vector<std::shared_ptr<DarlingServer::Thread>> permanentKernelAsyncRunners;

#define MAX_PERMANENT_KERNEL_RUNNERS 10

static void kernelAsyncRunnerThreadWorker(bool permanent, std::shared_ptr<DarlingServer::Thread> self) {
	do {
		// we're going to wait for work; we're available now.
		libsimple_lock_lock(&kernelAsyncRunnerQueueLock);
		++kernelAsyncRunnersAvailable;
		libsimple_lock_unlock(&kernelAsyncRunnerQueueLock);

		if (!dtape_semaphore_down_simple(kernelAsyncRunnerQueueSempahore)) {
			// we were interrupted. go again if we're permanent; otherwise, die.
			libsimple_lock_lock(&kernelAsyncRunnerQueueLock);
			--kernelAsyncRunnersAvailable;
			libsimple_lock_unlock(&kernelAsyncRunnerQueueLock);

			if (permanent) {
				continue;
			} else {
				break;
			}
		}

		libsimple_lock_lock(&kernelAsyncRunnerQueueLock);

		if (kernelAsyncRunnerQueue.empty()) {
			// we didn't find any work (we were probably awoken spuriously).
			// go again if we're permanent; otherwise, die.
			--kernelAsyncRunnersAvailable;
			libsimple_lock_unlock(&kernelAsyncRunnerQueueLock);

			if (permanent) {
				continue;
			} else {
				break;
			}
		}

		// we're going to perform some work; we're no longer available
		--kernelAsyncRunnersAvailable;

		auto func = kernelAsyncRunnerQueue.front();
		kernelAsyncRunnerQueue.pop();

		libsimple_lock_unlock(&kernelAsyncRunnerQueueLock);

		// perform the work
		func();
	} while (permanent);

	self = nullptr;
	DarlingServer::Thread::currentThread()->terminate();
	__builtin_unreachable();
};

void DarlingServer::Thread::kernelAsync(std::function<void()> fn) {
	static bool inited = []() {
		kernelAsyncRunnerQueueSempahore = dtape_semaphore_create(Process::kernelProcess()->_dtapeTask, 0);
		return true;
	}();

	libsimple_lock_lock(&kernelAsyncRunnerQueueLock);
	kernelAsyncRunnerQueue.push(fn);
	if (kernelAsyncRunnersAvailable == 0) {
		// we need to get some work done, but there are no workers available.
		// if we have less workers than the max permanent number of workers,
		// let's spawn a permanent worker. otherwise, just spawn a temporary worker.
		auto thread = std::make_shared<Thread>(KernelThreadConstructorTag());
		auto permanent = permanentKernelAsyncRunners.size() < MAX_PERMANENT_KERNEL_RUNNERS;
		thread->startKernelThread(std::bind(kernelAsyncRunnerThreadWorker, permanent, thread));
		if (permanent) {
			permanentKernelAsyncRunners.push_back(std::move(thread));
		}
	}
	libsimple_lock_unlock(&kernelAsyncRunnerQueueLock);

	// increment the semaphore to let workers know there's work available.
	dtape_semaphore_up(kernelAsyncRunnerQueueSempahore);
};

void DarlingServer::Thread::kernelSync(std::function<void()> fn) {
	std::mutex mutex;
	std::condition_variable condvar;
	bool done = false;

	kernelAsync([&]() {
		fn();

		{
			std::unique_lock lock2(mutex);
			done = true;
		}

		// notify all, but there should only be one thread waiting
		condvar.notify_all();
	});

	{
		std::unique_lock lock(mutex);
		condvar.wait(lock, [&]() {
			return done;
		});
	}
};

std::shared_ptr<DarlingServer::Thread> DarlingServer::Thread::threadForPort(uint32_t thread_port) {
	// prevent the target thread from dying by taking the global thread registry lock
	auto registryLock = threadRegistry().scopedLock();

	dtape_thread_t* thread_handle = dtape_thread_for_port(thread_port);
	if (!thread_handle) {
		return nullptr;
	}

	Thread* thread = static_cast<Thread*>(dtape_thread_context(thread_handle));
	if (!thread) {
		return nullptr;
	}

	return thread->shared_from_this();
};

void DarlingServer::Thread::loadStateFromUser(uint64_t threadState, uint64_t floatState) {
	int ret = dtape_thread_load_state_from_user(_dtapeThread, threadState, floatState);
	if (ret != 0) {
		throw std::system_error(-ret, std::generic_category());
	}
};

void DarlingServer::Thread::saveStateToUser(uint64_t threadState, uint64_t floatState) {
	int ret = dtape_thread_save_state_to_user(_dtapeThread, threadState, floatState);
	if (ret != 0) {
		throw std::system_error(-ret, std::generic_category());
	}
};

int DarlingServer::Thread::pendingSignal() const {
	std::shared_lock lock(_rwlock);
	return (_interrupts.empty()) ? 0 : _interrupts.top().signal;
};

int DarlingServer::Thread::setPendingSignal(int signal) {
	std::unique_lock lock(_rwlock);
	int pendingSignal;
	if (_interrupts.empty()) {
		throw std::runtime_error("Can't set pending signal with no active interrupts");
	} else {
		pendingSignal = _interrupts.top().signal;
		_interrupts.top().signal = signal;
	}
	return pendingSignal;
};

void DarlingServer::Thread::processSignal(int bsdSignalNumber, int linuxSignalNumber, int code, uintptr_t signalAddress, uintptr_t threadStateAddress, uintptr_t floatStateAddress) {
	loadStateFromUser(threadStateAddress, floatStateAddress);

	{
		std::unique_lock lock(_rwlock);
		if (!_interrupts.empty()) {
			_interrupts.top().signal = 0;
		} else {
			microthreadLog.error() << _tid << "(" << _nstid << "): processSignal with empty interrupt stack (desync)" << microthreadLog.endLog;
		}
		_processingSignal = true;
	}

	struct StandardSignalPendingClear {
		Thread* thread;
		int linuxSignalNumber;
		~StandardSignalPendingClear() {
			if (linuxSignalNumber > 0 && linuxSignalNumber < 32) {
				std::unique_lock lock(thread->_rwlock);
				thread->_pendingStandardSignalMask &= ~(1ull << linuxSignalNumber);
			}
		}
	} clearPendingStandardSignal{this, linuxSignalNumber};

	dtape_thread_process_signal(_dtapeThread, bsdSignalNumber, linuxSignalNumber, code, signalAddress);

	// LLDB commonly suspends the thread upon reception of an exception and assumes
	// that the thread will stay suspended after replying to the exception message,
	// until thread_resume() is called.
	dtape_thread_wait_while_user_suspended(_dtapeThread);

	{
		std::unique_lock lock(_rwlock);
		_processingSignal = false;
	}

	saveStateToUser(threadStateAddress, floatStateAddress);
};

void DarlingServer::Thread::handleSignal(int signal) {
	std::unique_lock lock(_rwlock);
	if (_processingSignal) {
		if (!_interrupts.empty()) {
			_interrupts.top().signal = signal;
		} else {
			microthreadLog.error() << _tid << "(" << _nstid << "): handleSignal with empty interrupt stack (desync); signal " << signal << " dropped" << microthreadLog.endLog;
		}
	} else {
		throw std::runtime_error("Attempt to handle signal while not processing signal");
	}
};

void DarlingServer::Thread::setPendingCallOverride(bool pendingCallOverride) {
	std::unique_lock lock(_rwlock);
	_pendingCallOverride = pendingCallOverride;
};

/*
 * server-to-client (S2C) calls are used by darlingserver to invoke certain functions within managed processes
 * for which there is no in-server alterative.
 *
 * for example, memory allocation can only be done by the managed process itself; there is no Linux syscall to allocate memory in another process.
 * therefore, we have to ask the process to do it for us.
 *
 * an alternative to this system is ptrace. we can attach to the managed process and execute any function we like.
 * this is made even easier by the fact that we have our own code in the managed process, meaning we can help out the server by
 * providing thunks for it to execute that already include a debug trap.
 * the problem with this alternative is that there's no good way to tell when the child is done executing the function:
 *   * we could block with waitpid, but then that would block the worker thread for an indeterminate amount of time (and we want to avoid that).
 *   * we could have the main event loop poll periodically, but polling is undesirable.
 * additionally, if someone else is already ptracing that process, we lose the ability to execute code with this approach.
 * therefore, we have this RPC-based system instead.
 *
 * in order to perform an S2C call, however, the target thread MUST be waiting for a message from the server.
 * thus, when we want to perform an S2C call on a thread that isn't waiting for a message, we send it a real-time signal
 * to ask it to execute the S2C call. the signal is handled with the normal wrappers (interrupt_enter and interrupt_exit)
 * to properly handle the case when we may be accidentally interrupting an ongoing call in the thread (since we may have raced
 * with thread trying to perform a server call).
 */

static DarlingServer::Log s2cLog("s2c");

std::optional<DarlingServer::Message> DarlingServer::Thread::_s2cPerform(Message&& call, dserver_s2c_msgnum_t expectedReplyNumber, size_t expectedReplySize) {
	std::optional<Message> reply = std::nullopt;
	bool usingInterrupt = false;

	// make sure we're the only one performing an S2C call on this thread
	if (!dtape_semaphore_down_simple(_s2cPerformSempahore)) {
		// got interrupted while waiting
		return std::nullopt;
	}

	s2cLog.debug() << *this << ": Going to perform S2C call" << s2cLog.endLog;

	{
		std::unique_lock lock(_rwlock);

		if (!_activeCall) {
			// signal the thread that we want to perform an S2C call and wait for it to give us the green light
			lock.unlock();
			s2cLog.debug() << *this << ": Sending S2C signal" << s2cLog.endLog;
			usingInterrupt = true;
			sendSignal(LINUX_SIGRTMIN + 1);
			if (!dtape_semaphore_down_simple(_s2cInterruptEnterSemaphore)) {
				// got interrupted while waiting
				dtape_semaphore_up(_s2cPerformSempahore);
				return std::nullopt;
			}
			s2cLog.debug() << *this << ": Got green light to perform S2C call" << s2cLog.endLog;
			lock.lock();
		} else if (currentThread().get() != this) {
			// we have an active call, so the client is waiting for a reply and is able to perform an S2C call,
			// but we're not the active thread. thus, in order to guarantee the client doesn't receive a reply
			// and stop waiting before we get a chance to perform our S2C call, let's make sure replies are deferred.
			_deferReplyForS2C = true;
		}

#ifdef DSERVER_RING_TRANSPORT
		// perf #18 P8 D6 (caller-S2C sideband) STEP 1 ATTRIBUTION: this S2C is going to THIS thread (a
		// caller-S2C) iff currentThread()==this. Classify the active parent op's lane so we learn whether
		// the reachable caller-S2C munmap hazard is carried by a RING parent (curable by the sideband) or a
		// UDS parent (future hazard) or no managed call (server-internal). Read under the held _rwlock.
		{
			auto hdr = reinterpret_cast<const dserver_s2c_callhdr_t*>(call.data().data());
			bool isMunmap = (hdr->s2c_number == dserver_s2c_msgnum_munmap);
			bool toCurrentCaller = (currentThread().get() == this);
			if (isMunmap && toCurrentCaller) {
				// classify the active parent op's lane (the D6 attribution: which lane carries a caller-S2C
				// munmap). RING parent => curable by the duplex sideband; UDS parent => today not a ring-
				// deadlock (caller services via recvmsg) but a FUTURE hazard if that op is ever ring-
				// migrated; no managed call => pure server-internal.
				if (_ringReplyPending) {
					Metrics::shared().s2cMunmapRingParent.fetch_add(1, std::memory_order_relaxed);
				} else if (_activeCall) {
					Metrics::shared().s2cMunmapUdsParent.fetch_add(1, std::memory_order_relaxed);
				} else {
					Metrics::shared().s2cMunmapNoParent.fetch_add(1, std::memory_order_relaxed);
				}
			}
		}
#endif

		// perf #18 D9 (dar-1il.4) + D14 (dar-1il.9): record that the active call drove a CALLER-S2C
		// upcall (an S2C to THIS thread, i.e. while it is the caller waiting for a reply). Any caller-S2C
		// means the op is NOT a simple closed req->reply -- it needs the duplex lane (Lane 2), not the
		// simple ring (Lane 1) -- so the heatmap must flag it regardless of S2C type or ring build. D14
		// replaces the old sticky per-thread _heatmapCallDidS2c bool (consumed at the NEXT recordCall,
		// which over-attributed an exec/teardown munmap to the wrong op -- D13's mldr_path false latch)
		// with PER-CALL-SCOPED attribution: bump the bucket for the op ACTUALLY executing right now
		// (_activeCall), at the moment the S2C fires. _activeCall is null for a server-internal S2C (no
		// managed parent) -- already counted by s2cMunmapNoParent, no per-op bucket to charge.
		if (currentThread().get() == this && _activeCall) {
			Metrics::shared().recordCallerS2cFor(static_cast<uint32_t>(_activeCall->number()));
		}

		call.setAddress(_address);
	}

	// at least for now, in order to wait for the S2C reply, we need the calling thread to be a microthread,
	// so that waiting on the duct-taped semaphore will work
	if (!currentThread()) {
		dtape_semaphore_up(_s2cPerformSempahore);
		throw std::runtime_error("Must be in a microthread (any microthread) to wait for S2C reply");
	}

	s2cLog.debug() << *this << ": Going to send S2C message" << s2cLog.endLog;

#ifdef DSERVER_RING_TRANSPORT
	// perf #18 P8 D4 (dar-1il.3.2.1): if this thread's current call is a DUPLEX PARENT (a ring-originated
	// mach_port_deallocate from a duplex-deallocate-capable caller) and the S2C is the munmap shape, route
	// the upcall through the duplex MAILBOX instead of the UDS send. The fiber then parks on
	// _s2cReplySempahore exactly as the UDS path below; the main-loop _drainDuplexReply harvests the
	// correlated munmap reply, synthesizes _s2cReply, and ups the semaphore. The op runs entirely on the
	// ring -- no UDS fallback after this point, so the destroy side effect is applied exactly once.
	//
	// The guard can DECLINE (no v5 ring / no DEALLOCATE cap / mailbox busy). That decline happens BEFORE
	// any mutation only at the ROUTING layer (ringServiceThread), NOT here -- by the time _s2cPerform runs
	// the dtape op has already begun mutating (we're mid-ipc_right_dealloc). So a decline HERE must NOT
	// silently fall back to a UDS S2C for a ring-parked caller (it can't service it -> the very deadlock
	// we're avoiding). Instead, a decline here is a hard error: we leave the in-flight markers clear and
	// fall through to the UDS send, which is correct ONLY if the caller is NOT ring-parked. Since
	// _ringDuplexParentActive implies a ring-parked caller, a decline here is a should-not-happen
	// (the routing layer already verified the cap + clean mailbox); we log + take the UDS path as a
	// last resort (the bounded guest wait then UDS-falls-back the op). In practice the guard passes.
	bool duplexUpcallTaken = false;
	if (_ringDuplexParentActive && expectedReplyNumber == dserver_s2c_msgnum_munmap) {
		auto* mcall = reinterpret_cast<const dserver_s2c_call_munmap_t*>(call.data().data());
		uint64_t addr = mcall->address;
		uint64_t len = mcall->length;
		std::unique_lock lock(_rwlock);
		duplexUpcallTaken = _s2cTryDuplexMunmapLocked(dserver_s2c_msgnum_munmap, addr, len, lock);
		if (!duplexUpcallTaken) {
			s2cLog.error() << *this << ": duplex munmap guard declined mid-op (mailbox busy?); "
			               << "falling through to UDS S2C (caller may be ring-parked)" << s2cLog.endLog;
		}
	}
	if (!duplexUpcallTaken)
#endif
	{
		// send the call (UDS S2C -- the unchanged path)
		Server::sharedInstance().sendMessage(std::move(call));
	}

	// now let's wait for the reply
	if (!dtape_semaphore_down_simple(_s2cReplySempahore)) {
		// got interrupted while waiting
		dtape_semaphore_up(_s2cPerformSempahore);
		return std::nullopt;
	}

	s2cLog.debug() << *this << ": Received S2C reply" << s2cLog.endLog;

	// extract the reply
	{
		std::unique_lock lock(_rwlock);

		if (!_s2cReply) {
			// impossible, but just in case
			dtape_semaphore_up(_s2cPerformSempahore);
			throw std::runtime_error("S2C reply semaphore incremented, but no reply present");
		}

		reply = std::move(_s2cReply);
		_s2cReply = std::nullopt;

		// if we had replies deferred, now's the time to send them
		if (_deferReplyForS2C) {
			_deferReplyForS2C = false;
			if (_deferredReply) {
#ifdef DSERVER_RING_TRANSPORT
				// perf #18 P5-bulk (dar-1il.1): a ring-originated call that triggered this S2C
				// upcall deferred its reply; it MUST go back onto the s2c ring, not UDS, or the
				// ring-waiting guest never wakes. _publishReplyToRingLocked consumes _ringReplyPending
				// and returns false (not a ring call) -> fall through to the UDS send below.
				if (!_publishReplyToRingLocked(*_deferredReply))
#endif
				{
					Server::sharedInstance().sendMessage(std::move(*_deferredReply));
				}
				_deferredReply = std::nullopt;
				// A0 RPC TRACE: the STASH-DEFERRED[s2c] reply is now flushed. Its absence for a tid that
				// logged STASH-DEFERRED is the stall signature (deferred reply never flushed).
				DarlingServer::__rpctrace("FLUSH-DEFERRED htid=%d nstid=%lld", id(), (long long)nsid());
			}
		}
	}

	s2cLog.debug() << *this << ": Done performing S2C call" << s2cLog.endLog;

	// we're done performing the call; allow others to have a chance at performing an S2C call on this thread
	dtape_semaphore_up(_s2cPerformSempahore);

	if (usingInterrupt) {
		// if we used the S2C signal to perform the call, then the s2c_perform call is currently waiting for us to finish;
		// let it know that we're done
		s2cLog.debug() << *this << ": Allowing thread to resume from S2C interrupt" << s2cLog.endLog;
		dtape_semaphore_up(_s2cInterruptExitSemaphore);
	}

	// partially validate the reply

	if (reply->data().size() != expectedReplySize) {
		throw std::runtime_error("Invalid S2C reply: unxpected size");
	}

	auto replyHeader = reinterpret_cast<dserver_s2c_replyhdr_t*>(reply->data().data());
	if (replyHeader->s2c_number != expectedReplyNumber) {
		throw std::runtime_error("Invalid S2C reply: unexpected S2C reply number");
	}

	return std::move(*reply);
};

#ifdef DSERVER_RING_TRANSPORT
// perf #18 P8 D3 (dar-1il.3.1.1): the GUARDED duplex publish for an S2C upcall -- the iron conjunction
// guard + the mailbox publish. This is the SHARED primitive: in D3 the synthetic sentinel parent op
// drives it (state-machine completion in the drain); in Phase E the real _s2cPerform of a duplex-
// eligible op (deallocate) will drive it the same way and complete by upping its parked fiber. Returns
// true iff the duplex path was taken (the upcall is now in flight); false means the guard declined and
// the caller MUST take its verbatim fallback (UDS for a real op; failure-reply for the synthetic op --
// never a fabricated success).
//
// MUST be entered with `lock` (a unique_lock on _rwlock) held. On success it leaves the in-flight
// markers armed and `lock` STILL HELD (the caller drops it); the drain side (_drainDuplexReply) takes
// _rwlock too, so the markers are published atomically vs. the harvest. The upcall mailbox publish is
// a release-store linearization point; the guest pump observes it with acquire.
bool DarlingServer::Thread::_s2cTryDuplexLocked(uint32_t upcallOp, uint32_t arg, std::unique_lock<std::shared_mutex>& lock) {
	(void)lock; // documents the lock contract; we operate under the held lock
	// --- the iron conjunction guard: duplex ONLY if ALL hold, else the caller falls back ----------
	// (2) the caller has a v4+ ring that advertised the SELFTEST duplex capability.
	auto ring = _ring; // _ring is guarded by _rwlock; we hold it
	if (!ring || !ring->duplexCapable(DSERVER_RING_DUPLEX_CAP_SELFTEST)) {
		return false;
	}
	dserver_ring_shm_t* cb = ring->liveControlBlock();
	if (!cb) {
		return false;
	}
	// (4) the upcall shape is one the minimal protocol supports.
	if (upcallOp != DSERVER_RING_DUPLEX_UPCALL_ECHO) {
		return false;
	}
	// (5) one-outstanding invariant: no upcall already in flight on this thread, and the mailbox slot
	//     is free (no stale unconsumed upcall/reply). A stale ready bit -> decline (don't clobber).
	if (_duplexUpcallInFlight) {
		return false;
	}
	if (dserver_ring_duplex_upcall_available(cb)) {
		return false; // a prior upcall is still unhandled in the mailbox -> not safe to publish
	}
	if (__atomic_load_n(&cb->duplex_reply_ready, __ATOMIC_ACQUIRE) != 0u) {
		return false; // a stale reply sits unconsumed -> decline rather than risk mis-correlation
	}
	// (3)+(6) allocate NONZERO, monotonic correlation ids (ABA-safe: never reused while a stale reply
	//     could exist, because we just checked the mailbox is clean and we bump per upcall).
	uint32_t parentId = _duplexNextId++;
	uint32_t upcallId = _duplexNextId++;
	if (parentId == 0) { parentId = _duplexNextId++; } // wrap guard: 0 means "none"
	if (upcallId == 0) { upcallId = _duplexNextId++; }

	// Arm the in-flight markers BEFORE publishing the upcall, so the drain (which takes _rwlock) can
	// only ever observe a consistent (in-flight + correlation ids) state. (1)+(7) the caller-is-in-
	// duplex-wait and same-ring-instance conditions are intrinsic here: this IS the caller thread's
	// own ring, and the upcall targets that same ring's mailbox -- there is no cross-thread or
	// cross-ring delivery in this path.
	_duplexParentId = parentId;
	_duplexUpcallId = upcallId;
	_duplexReplyReady = false;
	_duplexUpcallInFlight = true;
	_duplexInFlightFast.store(true, std::memory_order_release); // lock-free mirror for the hot drain

	// publish the upcall (release-store linearization) while still holding the lock.
	dserver_ring_duplex_publish_upcall(cb, upcallOp, parentId, upcallId, arg);

	// the guest may be spinning its wait-pump (no syscall) or parked on the s2c futex; bump+conditional
	// FUTEX_WAKE exactly like a Lane-1 reply so a parked pump is woken without a syscall when it spins.
	ring->wakeGuest();

	return true;
};

// perf #18 P8 D3: harvest a correlated duplex reply for this thread, if one is in flight + ready, and
// COMPLETE the parent op. Called from the main-loop ring drain for every ring thread -- this is the
// resume point that keeps the server wait SCOPED to the op (no dedicated waiter thread, no blocking of
// the dserver). For the D3 synthetic sentinel, completion = publish the parent FINAL reply onto the
// s2c ring (routing #8). Returns true iff it did work (harvest or reject). Takes _rwlock itself.
bool DarlingServer::Thread::_drainDuplexReply() {
	// Hot path: the overwhelming common case is no duplex in flight on this thread. Skip the _rwlock
	// entirely with a single relaxed atomic load so the Lane-1 drain cost is unchanged (one load per
	// thread per drain, no lock). Only when a duplex upcall is genuinely in flight do we take the lock.
	if (!_duplexInFlightFast.load(std::memory_order_acquire)) {
		return false;
	}
	std::shared_ptr<RingBuffer> ringForReply;
	uint32_t finalSeq = 0;
	uint32_t finalResult = 0;
	int32_t  finalCode = 0;
	bool completeSentinel = false;
	bool resumeRealFiber = false; // perf #18 P8 D4: up _s2cReplySempahore for a real-op duplex parent
	{
		std::unique_lock lock(_rwlock);
		if (!_duplexUpcallInFlight) {
			return false; // nothing parked on a duplex reply
		}
		auto ring = _ring;
		if (!ring) {
			return false;
		}
		dserver_ring_shm_t* cb = ring->liveControlBlock();
		if (!cb) {
			return false;
		}
		int mismatch = 0;
		if (dserver_ring_duplex_reply_ready(cb, _duplexParentId, _duplexUpcallId, &mismatch)) {
			// correlated reply: harvest it, consume the slot, clear the in-flight state.
			_duplexReplyStatus = cb->duplex_reply_status;
			_duplexReplyArg = cb->duplex_reply_arg;
			int32_t replyErrno = cb->duplex_reply_errno;
			_duplexReplyReady = true;
			uint32_t realUpcallNum = _duplexRealUpcallNum;
			dserver_ring_duplex_consume_reply(cb);
			_duplexUpcallInFlight = false;
			_duplexRealUpcallNum = 0;
			_duplexUpcallDeadlineNs = 0; // perf #18 P8 D6: disarm the fail-closed deadline on success
			_duplexInFlightFast.store(false, std::memory_order_release);
			Metrics::shared().ringDuplexS2c.fetch_add(1, std::memory_order_relaxed);
			if (_duplexSentinelSeq != 0) {
				// D3 synthetic sentinel completion: stage the parent final reply (published below, lock
				// dropped). The parent op's result IS the echo result the guest computed for the upcall.
				completeSentinel = true;
				ringForReply = ring;
				finalSeq = _duplexSentinelSeq;
				finalResult = _duplexReplyArg;
				finalCode = (_duplexReplyStatus == 0) ? 0 : -1;
				_duplexSentinelSeq = 0;
			} else if (realUpcallNum == (uint32_t)dserver_s2c_msgnum_munmap) {
				// perf #18 P8 D5 (dar-1il.3.2.2) boot-scoped proof: a REAL caller-S2C munmap just completed
				// over the duplex mailbox for a vm_deallocate proof parent. Count it + mark the sticky flag
				// so the dispatcher spends a proof-budget unit (auto-disarm). THIS is the live caller-S2C
				// cure the duplex lane was built for, finally on the real op.
				if (_ringDuplexVmdeallocProof) {
					Metrics::shared().ringDuplexVmdeallocS2c.fetch_add(1, std::memory_order_relaxed);
					_ringDuplexVmdeallocS2cFired = true;
				}
				// perf #18 P8 D4: REAL-op resume. Synthesize the _s2cReply Message exactly as the UDS S2C
				// reply would arrive (a dserver_s2c_reply_munmap_t carrying the guest's munmap result), so
				// _s2cPerform's reply-extraction + validation path is byte-identical to UDS. Then up
				// _s2cReplySempahore to resume the parked fiber, which finishes the deallocate and emits
				// its final reply via the existing deferred-reply->ring path.
				Message s2cReply(sizeof(dserver_s2c_reply_munmap_t), 0);
				auto* r = reinterpret_cast<dserver_s2c_reply_munmap_t*>(s2cReply.data().data());
				r->header.call_number = 0;
				r->header.pid = 0;
				r->header.tid = 0;
				r->header.architecture = 0;
				r->header.s2c_number = dserver_s2c_msgnum_munmap;
				r->return_value = _duplexReplyStatus; // guest's munmap return_value (0 ok / -1 error)
				r->errno_result = replyErrno;
				if (_s2cReply) {
					// should be impossible (one outstanding upcall) -- but never clobber a pending reply.
					s2cLog.error() << *this << ": duplex munmap reply but an _s2cReply was already pending" << s2cLog.endLog;
				} else {
					_s2cReply = std::move(s2cReply);
					resumeRealFiber = true;
				}
			}
		} else if (mismatch) {
			// a reply landed that does NOT correlate (wrong parent/upcall id: a buggy/hostile guest or
			// a stale reply from a torn-down op). Refuse to resume on it: drop the slot, count it, and
			// FAIL the parent (no wedge, no fabricated success). pitfall #2 (ABA) + #3 (wrong corr).
			uint32_t realUpcallNum = _duplexRealUpcallNum;
			dserver_ring_duplex_consume_reply(cb);
			_duplexUpcallInFlight = false;
			_duplexRealUpcallNum = 0;
			_duplexUpcallDeadlineNs = 0; // perf #18 P8 D6: disarm the fail-closed deadline on mis-correlation
			_duplexInFlightFast.store(false, std::memory_order_release);
			Metrics::shared().ringDuplexReject.fetch_add(1, std::memory_order_relaxed);
			if (_duplexSentinelSeq != 0) {
				completeSentinel = true;
				ringForReply = ring;
				finalSeq = _duplexSentinelSeq;
				finalResult = 0;
				finalCode = -1; // mis-correlation -> the synthetic parent fails
				_duplexSentinelSeq = 0;
			} else if (realUpcallNum == (uint32_t)dserver_s2c_msgnum_munmap) {
				// perf #18 P8 D4: a mis-correlated reply for a REAL munmap upcall. We must NOT resume the
				// fiber on a bogus munmap result (that could tell the kernel the unmap succeeded when it
				// didn't = corruption). Synthesize a FAILED munmap reply (return_value=-1, EINTR) so the
				// fiber resumes, the deallocate sees the S2C failed, and the op surfaces an error rather
				// than fabricating success. Bounded: the fiber resumes immediately (no wedge).
				Message s2cReply(sizeof(dserver_s2c_reply_munmap_t), 0);
				auto* r = reinterpret_cast<dserver_s2c_reply_munmap_t*>(s2cReply.data().data());
				r->header.call_number = 0;
				r->header.pid = 0;
				r->header.tid = 0;
				r->header.architecture = 0;
				r->header.s2c_number = dserver_s2c_msgnum_munmap;
				r->return_value = -1;
				r->errno_result = 4 /*EINTR*/;
				if (!_s2cReply) {
					_s2cReply = std::move(s2cReply);
					resumeRealFiber = true;
				}
			}
		} else {
			// in flight, no correlated reply yet. perf #18 P8 D6: enforce the SERVER-side fail-closed
			// deadline (gist failure policy #12). If the guest never pumped this S2C by the deadline,
			// resume the fiber with a FAILED reply (so the op surfaces an error, never a leaked fiber /
			// fabricated success), count the timeout, clear the in-flight + mailbox state, and disarm the
			// vm_deallocate proof so a stuck guest can't keep re-arming. Bounded: the fiber resumes here.
			if (_duplexUpcallDeadlineNs != 0) {
				struct timespec ts;
				clock_gettime(CLOCK_MONOTONIC, &ts);
				uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
				if (now >= _duplexUpcallDeadlineNs) {
					uint32_t realUpcallNum = _duplexRealUpcallNum;
					// clear the mailbox upcall slot so a late guest pump can't act on a stale upcall, and
					// reset all in-flight markers.
					dserver_ring_duplex_consume_reply(cb); // idempotent clear of any partial reply state
					__atomic_store_n(&cb->duplex_upcall_ready, 0u, __ATOMIC_RELEASE); // retract the unhandled upcall
					_duplexUpcallInFlight = false;
					_duplexRealUpcallNum = 0;
					_duplexUpcallDeadlineNs = 0;
					_duplexInFlightFast.store(false, std::memory_order_release);
					Metrics::shared().ringDuplexVmdeallocTimeout.fetch_add(1, std::memory_order_relaxed);
					if (_ringDuplexVmdeallocProof) {
						// disarm the boot/synthetic proof so a non-pumping guest can't keep arming it.
						Metrics::shared().ringDuplexVmdeallocDisarmed.fetch_add(1, std::memory_order_relaxed);
					}
					s2cLog.error() << *this << ": [D6] duplex S2C upcall TIMED OUT (no guest pump by deadline); "
					               << "failing the parent op closed (bounded, no leaked fiber)" << s2cLog.endLog;
					if (_duplexSentinelSeq != 0) {
						completeSentinel = true;
						ringForReply = ring;
						finalSeq = _duplexSentinelSeq;
						finalResult = 0;
						finalCode = -1;
						_duplexSentinelSeq = 0;
					} else if (realUpcallNum == (uint32_t)dserver_s2c_msgnum_munmap) {
						Message s2cReply(sizeof(dserver_s2c_reply_munmap_t), 0);
						auto* r = reinterpret_cast<dserver_s2c_reply_munmap_t*>(s2cReply.data().data());
						r->header.call_number = 0;
						r->header.pid = 0;
						r->header.tid = 0;
						r->header.architecture = 0;
						r->header.s2c_number = dserver_s2c_msgnum_munmap;
						r->return_value = -1;
						r->errno_result = 110 /*ETIMEDOUT*/;
						if (!_s2cReply) {
							_s2cReply = std::move(s2cReply);
							resumeRealFiber = true;
						}
					}
				} else {
					return false; // in flight, deadline not yet reached
				}
			} else {
				return false; // in flight but no reply yet (no deadline armed -- e.g. D3 sentinel echo)
			}
		}
	}

	if (resumeRealFiber) {
		// resume the parked deallocate fiber with the synthesized munmap reply (the UDS path's wakeup).
		dtape_semaphore_up(_s2cReplySempahore);
		return true;
	}

	if (completeSentinel && ringForReply) {
		// publish the synthetic parent's final reply onto the s2c ring. Reply convention mirrors the
		// Lane-1 port-trap reply ({reply_hdr.code, uint32 result}); the guest selftest reads `result`.
		uint32_t body = finalResult;
		if (!ringForReply->publishReply(finalSeq, DSERVER_RING_DUPLEX_SELFTEST_CALLNUM, finalCode, &body, (uint32_t)sizeof(body))) {
			// s2c full: the guest will time out on the parent + UDS-fall-back in a real lane. For the
			// synthetic selftest this is a transient miss; count it as an s2c-full like Lane-1.
			Metrics::shared().ringS2cFull.fetch_add(1, std::memory_order_relaxed);
		}
		ringForReply->wakeGuest();
		return true;
	}
	return true;
};

bool DarlingServer::Thread::drainDuplexReply() {
	return _drainDuplexReply();
};

// perf #18 P8 D3: issue ONE synthetic duplex ECHO upcall for the sentinel parent op (seq = the parent
// request's ring seq). Runs the guarded publish; if the guard passes, the upcall is in flight and the
// parent reply will be published by _drainDuplexReply when the correlated reply arrives. Returns true
// if the duplex path was taken (parent reply deferred to the drain); false if the guard declined, in
// which case the CALLER must publish the synthetic parent's failure reply immediately (no fabricated
// success). NOT a microthread/fiber path -- this is a synchronous state-machine kickoff from the ring
// service loop, so it never parks (the brief's "publish upcall + return; later drain completes").
bool DarlingServer::Thread::duplexSelftestUpcall(uint32_t arg, uint32_t seq) {
	std::unique_lock lock(_rwlock);
	if (_duplexUpcallInFlight) {
		return false; // one-outstanding: a prior selftest is mid-flight on this thread
	}
	_duplexSentinelSeq = seq; // mark this in-flight upcall as a sentinel parent (drain completes it)
	bool took = _s2cTryDuplexLocked(DSERVER_RING_DUPLEX_UPCALL_ECHO, arg, lock);
	if (!took) {
		_duplexSentinelSeq = 0; // guard declined -> not in flight; caller publishes the failure reply
	}
	return took;
};

// perf #18 P8 D4 (dar-1il.3.2.1): publish a REAL munmap S2C upcall into the duplex mailbox for a duplex-
// parent call. The structural sibling of _s2cTryDuplexLocked, but for the MUNMAP shape + the typed
// addr/len payload, and it ALSO requires DUPLEX_CAP_DEALLOCATE (the echo guard required only SELFTEST).
// On success it arms the in-flight markers + publishes + wakes, and the CALLER (_s2cPerform) then parks
// the fiber on _s2cReplySempahore exactly as the UDS S2C path does. The main-loop _drainDuplexReply
// harvests the correlated munmap reply, synthesizes the _s2cReply Message, and ups _s2cReplySempahore.
bool DarlingServer::Thread::_s2cTryDuplexMunmapLocked(uint32_t s2cNumber, uint64_t address, uint64_t length, std::unique_lock<std::shared_mutex>& lock) {
	(void)lock; // documents the held-lock contract
	auto ring = _ring;
	// require a v5+ duplex-capable ring AND a munmap-pump cap. The munmap shape is reachable from EITHER a
	// (D4) mach_port_deallocate parent OR a (D5) vm_deallocate parent; both advertise they can pump munmap,
	// via CAP_DEALLOCATE (0x2) or CAP_VM_DEALLOCATE (0x4). SELFTEST alone is not enough. The per-op routing
	// decline (ringServiceThread) already keyed on the SPECIFIC bit before dispatch, so by the time we
	// publish a munmap upcall the caller has the matching cap; this guard is the belt-and-suspenders check.
	if (!ring || !ring->duplexCapable(DSERVER_RING_DUPLEX_CAP_MUNMAP_PUMP)) {
		return false;
	}
	dserver_ring_shm_t* cb = ring->liveControlBlock();
	if (!cb) {
		return false;
	}
	// one-outstanding invariant: nothing already in flight + a clean mailbox (no stale upcall/reply).
	if (_duplexUpcallInFlight) {
		return false;
	}
	if (dserver_ring_duplex_upcall_available(cb)) {
		return false;
	}
	if (__atomic_load_n(&cb->duplex_reply_ready, __ATOMIC_ACQUIRE) != 0u) {
		return false;
	}
	// nonzero, monotonic correlation ids (ABA-safe: the mailbox is clean + we bump per upcall).
	uint32_t parentId = _duplexNextId++;
	uint32_t upcallId = _duplexNextId++;
	if (parentId == 0) { parentId = _duplexNextId++; }
	if (upcallId == 0) { upcallId = _duplexNextId++; }

	_duplexParentId = parentId;
	_duplexUpcallId = upcallId;
	_duplexReplyReady = false;
	_duplexUpcallInFlight = true;
	_duplexRealUpcallNum = s2cNumber; // mark this as a REAL-op upcall (drain synthesizes _s2cReply)
	_duplexInFlightFast.store(true, std::memory_order_release);
	// perf #18 P8 D6: arm the SERVER-side fail-closed deadline (gist failure policy #12). If the guest
	// never pumps this munmap S2C (a buggy/torn-down/RED-no-pump guest), the main-loop drain resumes the
	// fiber with a FAILED reply at the deadline instead of leaking the fiber forever. Generous (5s) -- a
	// real pump replies in microseconds, so this never trips in GREEN; it bounds only the pathological case.
	{
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		_duplexUpcallDeadlineNs = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec + 5000000000ull;
	}

	dserver_ring_duplex_publish_munmap_upcall(cb, parentId, upcallId, address, length);
	ring->wakeGuest();
	return true;
};

void DarlingServer::Thread::setRingDuplexParentActive(bool active) {
	std::unique_lock lock(_rwlock);
	_ringDuplexParentActive = active;
};

// perf #18 P8 D5 (dar-1il.3.2.2) boot-scoped proof.
void DarlingServer::Thread::setRingDuplexVmdeallocProof(bool active) {
	std::unique_lock lock(_rwlock);
	_ringDuplexVmdeallocProof = active;
	if (active) {
		_ringDuplexVmdeallocS2cFired = false; // reset the per-dispatch sticky flag at arm time
	}
};

bool DarlingServer::Thread::takeRingDuplexVmdeallocS2cFired() {
	std::unique_lock lock(_rwlock);
	bool v = _ringDuplexVmdeallocS2cFired;
	_ringDuplexVmdeallocS2cFired = false;
	return v;
};

bool DarlingServer::Thread::duplexDeallocateCapable() const {
	std::unique_lock lock(const_cast<std::shared_mutex&>(_rwlock));
	auto ring = _ring;
	if (!ring) {
		return false;
	}
	return ring->duplexCapable(DSERVER_RING_DUPLEX_CAP_DEALLOCATE);
};

// perf #18 P8 D5 (dar-1il.3.2.2): the per-op routing gate for vm_deallocate. Keys on the SPECIFIC
// VM_DEALLOCATE cap bit (not the shared munmap-pump mask): a guest that advertised only the (D4)
// DEALLOCATE cap must NOT have a vm_deallocate routed onto the lane on its behalf.
bool DarlingServer::Thread::duplexVmDeallocateCapable() const {
	std::unique_lock lock(const_cast<std::shared_mutex&>(_rwlock));
	auto ring = _ring;
	if (!ring) {
		return false;
	}
	return ring->duplexCapable(DSERVER_RING_DUPLEX_CAP_VM_DEALLOCATE);
};
#endif // DSERVER_RING_TRANSPORT

uintptr_t DarlingServer::Thread::_mmap(uintptr_t address, size_t length, int protection, int flags, int fd, off_t offset, int& outErrno) {
	// XXX: not sure if we want to force all allocations in 32-bit processes to be in the 32-bit address space.
	//      for now, we leave it up to the caller.
#if 0
	auto process = _process.lock();

	if (!process) {
		throw std::runtime_error("Cannot perform mmap without valid process");
	}

	if (process->architecture() == Process::Architecture::i386 || process->architecture() == Process::Architecture::ARM32) {
		flags |= MAP_32BIT;
	}
#endif

	Message callMessage(sizeof(dserver_s2c_call_mmap_t), (fd < 0) ? 0 : 1);
	auto call = reinterpret_cast<dserver_s2c_call_mmap_t*>(callMessage.data().data());

	call->header.call_number = dserver_callnum_s2c;
	call->header.s2c_number = dserver_s2c_msgnum_mmap;
	call->address = address;
	call->length = length;
	call->protection = protection;
	call->flags = flags;
	call->fd = (fd < 0) ? -1 : 0;
	call->offset = offset;

	if (fd >= 0) {
		auto dupfd = dup(fd);
		if (dupfd < 0) {
			outErrno = errno;
			return (uintptr_t)MAP_FAILED;
		}
		callMessage.pushDescriptor(dupfd);
	}

	s2cLog.debug() << "Performing _mmap with address=" << call->address << ", length=" << call->length << ", protection=" << call->protection << ", flags=" << call->flags << ", fd=" << call->fd << " (" << fd << ")" << ", offset=" << call->offset << s2cLog.endLog;

	auto maybeReplyMessage = _s2cPerform(std::move(callMessage), dserver_s2c_msgnum_mmap, sizeof(dserver_s2c_reply_mmap_t));
	if (!maybeReplyMessage) {
		s2cLog.debug() << "_mmap call interrupted" << s2cLog.endLog;
		outErrno = EINTR;
		return (uintptr_t)MAP_FAILED;
	}

	auto replyMessage = std::move(*maybeReplyMessage);
	auto reply = reinterpret_cast<dserver_s2c_reply_mmap_t*>(replyMessage.data().data());

	s2cLog.debug() << "_mmap returned address=" << reply->address << ", errno_result=" << reply->errno_result << s2cLog.endLog;

	outErrno = reply->errno_result;
	return reply->address;
};

int DarlingServer::Thread::_munmap(uintptr_t address, size_t length, int& outErrno) {
	Message callMessage(sizeof(dserver_s2c_call_munmap_t), 0);
	auto call = reinterpret_cast<dserver_s2c_call_munmap_t*>(callMessage.data().data());

	call->header.call_number = dserver_callnum_s2c;
	call->header.s2c_number = dserver_s2c_msgnum_munmap;
	call->address = address;
	call->length = length;

	s2cLog.debug() << "Performing _munmap with address=" << call->address << ", length=" << call->length << s2cLog.endLog;

	auto maybeReplyMessage = _s2cPerform(std::move(callMessage), dserver_s2c_msgnum_munmap, sizeof(dserver_s2c_reply_munmap_t));
	if (!maybeReplyMessage) {
		s2cLog.debug() << "_munmap call interrupted" << s2cLog.endLog;
		outErrno = EINTR;
		return -1;
	}

	auto replyMessage = std::move(*maybeReplyMessage);
	auto reply = reinterpret_cast<dserver_s2c_reply_munmap_t*>(replyMessage.data().data());

	s2cLog.debug() << "_munmap returned return_value=" << reply->return_value << ", errno_result=" << reply->errno_result << s2cLog.endLog;

	outErrno = reply->errno_result;
	return reply->return_value;
};

int DarlingServer::Thread::_mprotect(uintptr_t address, size_t length, int protection, int& outErrno) {
	Message callMessage(sizeof(dserver_s2c_call_mprotect_t), 0);
	auto call = reinterpret_cast<dserver_s2c_call_mprotect_t*>(callMessage.data().data());

	call->header.call_number = dserver_callnum_s2c;
	call->header.s2c_number = dserver_s2c_msgnum_mprotect;
	call->address = address;
	call->length = length;
	call->protection = protection;

	s2cLog.debug() << "Performing _mprotect with address=" << call->address << ", length=" << call->length << ", protection=" << call->protection << s2cLog.endLog;

	auto maybeReplyMessage = _s2cPerform(std::move(callMessage), dserver_s2c_msgnum_mprotect, sizeof(dserver_s2c_reply_mprotect_t));
	if (!maybeReplyMessage) {
		s2cLog.debug() << "_mprotect call interrupted" << s2cLog.endLog;
		outErrno = EINTR;
		return -1;
	}

	auto replyMessage = std::move(*maybeReplyMessage);
	auto reply = reinterpret_cast<dserver_s2c_reply_mprotect_t*>(replyMessage.data().data());

	s2cLog.debug() << "_mprotect returned return_value=" << reply->return_value << ", errno_result=" << reply->errno_result << s2cLog.endLog;

	outErrno = reply->errno_result;
	return reply->return_value;
};

int DarlingServer::Thread::_msync(uintptr_t address, size_t size, int sync_flags, int& outErrno) {
	Message callMessage(sizeof(dserver_s2c_call_msync_t), 0);
	auto call = reinterpret_cast<dserver_s2c_call_msync_t*>(callMessage.data().data());

	call->header.call_number = dserver_callnum_s2c;
	call->header.s2c_number = dserver_s2c_msgnum_msync;
	call->address = address;
	call->size = size;
	call->sync_flags = sync_flags;

	s2cLog.debug() << "Performing _msync with address=" << call->address << ", size=" << call->size << ", sync_flags=" << call->sync_flags << s2cLog.endLog;

	auto maybeReplyMessage = _s2cPerform(std::move(callMessage), dserver_s2c_msgnum_msync, sizeof(dserver_s2c_reply_msync_t));
	if (!maybeReplyMessage) {
		s2cLog.debug() << "_msync call interrupted" << s2cLog.endLog;
		outErrno = EINTR;
		return -1;
	}

	auto replyMessage = std::move(*maybeReplyMessage);
	auto reply = reinterpret_cast<dserver_s2c_reply_msync_t*>(replyMessage.data().data());

	s2cLog.debug() << "_msync returned return_value=" << reply->return_value << ", errno_result=" << reply->errno_result << s2cLog.endLog;

	outErrno = reply->errno_result;
	return reply->return_value;
};

uintptr_t DarlingServer::Thread::allocatePages(size_t pageCount, int protection, uintptr_t addressHint, bool fixed, bool overwrite) {
	int err = 0;
	int flags = MAP_PRIVATE | MAP_ANONYMOUS;
	if (fixed && overwrite) {
		flags |= MAP_FIXED;
	} else if (fixed) {
		flags |= MAP_FIXED_NOREPLACE;
	}
	auto result = _mmap(addressHint, pageCount * sysconf(_SC_PAGESIZE), protection, flags, -1, 0, err);
	if (result == (uintptr_t)MAP_FAILED) {
		throw std::system_error(err, std::generic_category(), "S2C mmap call failed");
	}
	return result;
};

void DarlingServer::Thread::freePages(uintptr_t address, size_t pageCount) {
	int err = 0;
	if (_munmap(address, pageCount * sysconf(_SC_PAGESIZE), err) < 0) {
		throw std::system_error(err, std::generic_category(), "S2C munmap call failed");
	}
};

uintptr_t DarlingServer::Thread::mapFile(int fd, size_t pageCount, int protection, uintptr_t addressHint, size_t pageOffset, bool fixed, bool overwrite) {
	int err = 0;
	int flags = MAP_SHARED;
	if (fixed && overwrite) {
		flags |= MAP_FIXED;
	} else if (fixed) {
		flags |= MAP_FIXED_NOREPLACE;
	}
	auto result = _mmap(addressHint, pageCount * sysconf(_SC_PAGESIZE), protection, flags, fd, pageOffset * sysconf(_SC_PAGESIZE), err);
	if (result == (uintptr_t)MAP_FAILED) {
		throw std::system_error(err, std::generic_category(), "S2C mmap call failed");
	}
	return result;
};

void DarlingServer::Thread::changeProtection(uintptr_t address, size_t pageCount, int protection) {
	int err = 0;
	if (_mprotect(address, pageCount * sysconf(_SC_PAGESIZE), protection, err) < 0) {
		throw std::system_error(err, std::generic_category(), "S2C mprotect call failed");
	}
};

void DarlingServer::Thread::syncMemory(uintptr_t address, size_t size, int sync_flags) {
	int err = 0;
	if (_msync(address, size, sync_flags, err) < 0) {
		throw std::system_error(err, std::generic_category(), "S2C msync call failed");
	}
};

void DarlingServer::Thread::waitUntilRunning() {
	std::shared_lock lock(_rwlock);
	_runningCondvar.wait(lock, [&]() {
		return _isRunningLocked();
	});
};

void DarlingServer::Thread::waitUntilNotRunning() {
	std::shared_lock lock(_rwlock);
	_runningCondvar.wait(lock, [&]() {
		return !_isRunningLocked();
	});
};

void DarlingServer::Thread::_deferLocked(bool wait, std::unique_lock<std::shared_mutex>& lock) {
	if (_deferralState == DeferralState::NotDeferred) {
		_deferralState = DeferralState::DeferredNotPending;
	}

	if (wait) {
		_runningCondvar.wait(lock, [&]() {
			return !_isRunningLocked();
		});
	}
};

void DarlingServer::Thread::defer(bool wait) {
	std::unique_lock lock(_rwlock);
	_deferLocked(wait, lock);
};

void DarlingServer::Thread::_undeferLocked(std::unique_lock<std::shared_mutex>& lock) {
	DeferralState previousDeferralState;

	previousDeferralState = _deferralState;
	_deferralState = DeferralState::NotDeferred;

	if (previousDeferralState == DeferralState::DeferredPending) {
		Server::sharedInstance().scheduleThread(shared_from_this());
	}
};

void DarlingServer::Thread::undefer() {
	std::unique_lock lock(_rwlock);
	_undeferLocked(lock);
};

uint32_t* DarlingServer::Thread::bsdReturnValuePointer() {
	return &_bsdReturnValue;
};

void DarlingServer::Thread::logToStream(Log::Stream& stream) const {
	stream << "[T:" << _tid << "(" << _nstid << ")]";
};

#ifdef DSERVER_RING_TRANSPORT
// perf #18 P3 / P5-bulk: publish a complete reply Message onto the s2c ring if this thread's current
// call is ring-originated. Returns true if it took ownership of the reply (published, OR consumed it
// to UDS-fall-back on a full ring); false if this is not a ring call (caller must send it via UDS).
// Consumes _ringReplyPending. _rwlock MUST be held by the caller. The reply Message data is
// dserver_rpc_reply_<call>_t = {replyhdr{number,code}, body}; we carry the code + body bytes onto the
// ring (the guest reconstructs the same body).
bool DarlingServer::Thread::_publishReplyToRingLocked(Message& reply) {
	if (!_ringReplyPending || !_ring) {
		return false;
	}
	_ringReplyPending = false;
	const auto& bytes = reply.data();
	if (bytes.size() < sizeof(dserver_rpc_replyhdr_t)) {
		return false; // malformed (too short) -> caller UDS-falls-back
	}
	const dserver_rpc_replyhdr_t* rhdr = reinterpret_cast<const dserver_rpc_replyhdr_t*>(bytes.data());
	const uint8_t* body = bytes.data() + sizeof(dserver_rpc_replyhdr_t);
	uint32_t bodyLen = static_cast<uint32_t>(bytes.size() - sizeof(dserver_rpc_replyhdr_t));
	auto ring = _ring; // keep alive across the publish
	uint32_t seq = _ringReplySeq;
	uint32_t callnum = static_cast<uint32_t>(rhdr->number);
	int32_t code = rhdr->code;
	// publish under the lock is fine (no suspend, no Server-state mutation); the FUTEX_WAKE is a
	// bare syscall and likewise can't re-enter our locks.
#ifdef DSERVER_RING_PHASE_PROF
	uint64_t _pubT0 = Metrics::rdtscCycles();
#endif
	bool published = ring->publishReply(seq, callnum, code, body, bodyLen);
#ifdef DSERVER_RING_PHASE_PROF
	_ringPublishCycles = Metrics::rdtscCycles() - _pubT0; // read back by ringServiceThread
#endif
	if (!published) {
		// s2c full: fall back to a UDS reply so the guest still gets its answer.
		Metrics::shared().ringS2cFull.fetch_add(1, std::memory_order_relaxed);
		Server::sharedInstance().sendMessage(std::move(reply));
	} else {
		ring->wakeGuest();
	}
	return true;
}
#endif

void DarlingServer::Thread::pushCallReply(std::shared_ptr<Call> expectedCall, Message&& reply) {
	std::shared_ptr<Call> owedEnterReply = nullptr;
	{
	std::unique_lock lock(_rwlock);

	// A0-ARCH stage 3: a reply from the interrupted call of an OPEN interrupt frame stashes
	// on the frame (the guest flushes it at interrupt_exit) -- keyed on the durable frame
	// fact, not the old transient _interruptedForSignal window, because the cancelled call
	// now replies through the normal dispatch path AFTER interrupt_enter's processCall has
	// completed. Computed BEFORE deactivation (which nulls the frame slot).
	bool stashOnInterruptFrame = expectedCall && !_interrupts.empty()
		&& _interrupts.top().interruptedCall.get() == expectedCall.get();

	if (expectedCall) {
		_deactivateCallLocked(expectedCall);
	}

	// A0 RPC TRACE: name the disposition of this reply. Four outcomes, keyed by tid/nsid so it lines up
	// with the RECV line and the guest-capture /proc/<tid>. A stall reads directly off the tape: a RECV
	// whose reply ends in STASH-SAVED or STASH-DEFERRED with no later FLUSH-* line, or a RECV that never
	// produces any REPLY-* line, names the stuck call + drop site. (call number from expectedCall.)
	{
		unsigned callnum = expectedCall ? (unsigned)expectedCall->number() : 0u;
		const char* disp = stashOnInterruptFrame ? "STASH-SAVED[interrupt]"
			: _deferReplyForS2C ? "STASH-DEFERRED[s2c]"
			: _dead ? "DROP-DEAD"
			: "SEND";
		DarlingServer::__rpctrace("REPLY-DISP htid=%d nstid=%lld call=%u disp=%s",
			id(), (long long)nsid(), callnum, disp);
	}

	if (stashOnInterruptFrame) {
		if (_interrupts.top().savedReply) {
			throw std::runtime_error("New reply would overwrite existing saved reply");
		}

		_interrupts.top().savedReply = std::move(reply);

		// the interrupted call's reply is now safely stashed -- if interrupt_enter's reply
		// was deferred on it, it goes out NOW (outside the lock below): exactly the guest
		// ordering the old synchronous unwind provided, without borrowing any stack.
		// enterCall stays IN the frame slot: its sendBasicReply re-enters pushCallReply,
		// whose _deactivateCallLocked clears it from there.
		if (_interrupts.top().replyOwed) {
			_interrupts.top().replyOwed = false;
			owedEnterReply = _interrupts.top().enterCall;
		}
	} else if (_deferReplyForS2C) {
		// A ring-originated call that performs an S2C upcall defers its reply here; the flush in
		// _s2cPerform() (NOT this path) republishes it -- and MUST honor _ringReplyPending or a
		// ring-waiting guest wedges. We deliberately do NOT consume _ringReplyPending here so the
		// flush still knows the reply belongs to the ring. (perf #18 P5-bulk / dar-1il.1)
		_deferredReply = std::move(reply);
	} else if (!_dead) {
#ifdef DSERVER_RING_TRANSPORT
		if (_publishReplyToRingLocked(reply)) {
			DarlingServer::__rpctrace("SENT-RING htid=%d nstid=%lld", id(), (long long)nsid());
			return;
		}
#endif
		DarlingServer::__rpctrace("SENT-UDS htid=%d nstid=%lld", id(), (long long)nsid());
		Server::sharedInstance().sendMessage(std::move(reply));
	}
	} // release _rwlock

	if (owedEnterReply) {
		try {
			owedEnterReply->sendBasicReply(0);
		} catch (const std::exception& ex) {
			// only reachable under an RPC-stream desync that popped the frame between the
			// unlock above and the re-lock inside sendBasicReply; survive loudly
			microthreadLog.error() << _tid << "(" << _nstid
				<< "): deferred interrupt_enter reply failed: " << ex.what() << microthreadLog.endLog;
		}
	}
};

bool DarlingServer::Thread::isCurrentlySuspended() const {
	// perf #2b: read the same suspended view doWork()/suspend() maintain. A microthread
	// that ran to completion inline never parked; one that blocked reads suspended
	// (until a resume). Shared-lock is enough -- this is a single-field read.
	std::shared_lock lock(_rwlock);
	return _isSuspendedLocked();
};

DarlingServer::Thread::RunState DarlingServer::Thread::getRunState() const {
	auto process = this->process();
	if (!process || isDead()) {
		return RunState::Dead;
	}

	std::ifstream file("/proc/" + std::to_string(process->id()) + "/task/" + std::to_string(id()) + "/stat");
	std::string line;
	if (!std::getline(file, line)) {
		return RunState::Dead;
	}

	auto endOfComm = line.find(')');
	if (endOfComm == std::string::npos) {
		return RunState::Dead;
	}

	if (line.size() <= endOfComm + 2) {
		return RunState::Dead;
	}

	switch (line[endOfComm + 2]) {
		case 'R':
			return RunState::Running;
		case 'S':
			return RunState::Interruptible;
		case 'D':
			return RunState::Uninterruptible;
		case 'T':
			return RunState::Stopped;
		default:
			return RunState::Dead;
	}
};

void DarlingServer::Thread::waitWhileUserSuspended(uintptr_t threadStateAddress, uintptr_t floatStateAddress) {
	loadStateFromUser(threadStateAddress, floatStateAddress);
	dtape_thread_wait_while_user_suspended(_dtapeThread);
	try {
		saveStateToUser(threadStateAddress, floatStateAddress);
	} catch (std::system_error e) {
		// if we fail to save the state back to the process, that likely means the process died or was killed while waiting.
		// it's nothing to worry about. just log it and move on.
		threadLog.warning() << *this << ": failed to save state back to user in waitWhileUserSuspended: " << e.code() << " (" << e.what() << ")" << threadLog.endLog;
	}
};

void DarlingServer::Thread::sendSignal(int signal) const {
	if (isDead()) {
		return;
	}
	bool markedPending = false;
	if (signal > 0 && signal < 32 && signal != SIGCHLD && signal != SIGUSR1) {
		std::unique_lock lock(_rwlock);
		uint64_t bit = 1ull << signal;
		if (_pendingStandardSignalMask & bit) {
			return;
		}
		_pendingStandardSignalMask |= bit;
		markedPending = true;
	}
	if (_process) {
		if (syscall(SYS_tgkill, _process->id(), id(), signal) < 0) {
			int code = errno;
			if (markedPending) {
				std::unique_lock lock(_rwlock);
				_pendingStandardSignalMask &= ~(1ull << signal);
			}
			throw std::system_error(code, std::generic_category());
		}
	} else {
		if (markedPending) {
			std::unique_lock lock(_rwlock);
			_pendingStandardSignalMask &= ~(1ull << signal);
		}
		throw std::system_error(ESRCH, std::generic_category());
	}
};

void DarlingServer::Thread::notifyDead() {
	bool canRelease = false;

#ifdef DSERVER_RING_TRANSPORT
	// perf #18 (dar-dar6x4-perf-5dq.30): grab the ring + its Monitor out from under _rwlock
	// and release them AFTER unlocking -- removeMonitor() touches Server state and could
	// otherwise reintroduce the dar-6x4 lock-across-suspend hazard class. The RingBuffer dtor
	// unmaps + closes the eventfd.
	std::shared_ptr<RingBuffer> ringToRelease = nullptr;
	std::shared_ptr<Monitor> ringMonitorToRelease = nullptr;
#endif

	{
		std::unique_lock lock(_rwlock);
		if (_dead) {
			return;
		}

		threadLog.info() << *this << ": thread dying" << threadLog.endLog;
		_dead = true;
		_mstateEventLocked(StateEvent::Note, "notify-dead");

#ifdef DSERVER_RING_TRANSPORT
		ringMonitorToRelease = std::move(_ringMonitor);
		ringToRelease = std::move(_ring);
		_ringMonitor = nullptr;
		_ring = nullptr;
#endif

		if (!_activeCall) {
			// if we have no active call, we won't ever need to run again,
			// so set `_terminating` to make sure that doesn't happen
			_terminating = true;
			canRelease = true;
			_mstateTransitionLocked(MicroState::Terminated, "dead-no-active-call");
		}
	}

#ifdef DSERVER_RING_TRANSPORT
	// _rwlock is dropped now; remove ourselves from the main-loop spin registry (perf #18 P4),
	// tear down the ring's epoll Monitor, and release the mapping.
	if (ringToRelease) {
		Server::sharedInstance().unregisterRingThread(shared_from_this());
	}
	if (ringMonitorToRelease) {
		Server::sharedInstance().removeMonitor(ringMonitorToRelease);
	}
	ringMonitorToRelease = nullptr;
	ringToRelease = nullptr; // RingBuffer dtor: munmap + close eventfd
#endif

	// keep ourselves alive until the duct-taped context is done
	_selfReference = shared_from_this();

	dtape_thread_dying(_dtapeThread);

	if (canRelease) {
		_scheduleRelease();
	} else {
		// liveness abort: pop whatever park the dying thread is in, regardless of typed wait
		// state -- doWork routes a dead thread into its terminating paths
		wake(WakeKind::Abort, 0);
	}

	threadRegistry().unregisterEntry(shared_from_this());
};

bool DarlingServer::Thread::isDead() const {
	std::shared_lock lock(_rwlock);
	return _dead;
};

#ifdef DSERVER_RING_TRANSPORT
void DarlingServer::Thread::attachRing(std::shared_ptr<RingBuffer> ring, std::shared_ptr<Monitor> monitor) {
	std::shared_ptr<RingBuffer> oldRing = nullptr;
	std::shared_ptr<Monitor> oldMonitor = nullptr;
	{
		std::unique_lock lock(_rwlock);
		// a thread attaches at most once in practice, but replace defensively
		oldRing = std::move(_ring);
		oldMonitor = std::move(_ringMonitor);
		_ring = ring;
		_ringMonitor = monitor;
	}
	// drop any prior ring's Monitor outside the lock (same hazard discipline as notifyDead)
	if (oldMonitor) {
		Server::sharedInstance().removeMonitor(oldMonitor);
	}
};

std::shared_ptr<DarlingServer::RingBuffer> DarlingServer::Thread::ring() const {
	std::shared_lock lock(_rwlock);
	return _ring;
};

void DarlingServer::Thread::beginRingReply(uint32_t seq) {
	std::unique_lock lock(_rwlock);
	_ringReplyPending = true;
	_ringReplySeq = seq;
	// perf #18 D9: latch that this call is ring-originated so the heatmap can attribute its transport
	// at recordCall time (by then _ringReplyPending has been consumed by the reply publish).
	_heatmapCallWasRing = true;
};
#ifdef DSERVER_RING_PHASE_PROF
uint64_t DarlingServer::Thread::takeRingPublishCycles() {
	// no lock: only the main loop touches this, in the same ringServiceThread call chain.
	uint64_t v = _ringPublishCycles;
	_ringPublishCycles = 0;
	return v;
};
#endif
#endif

void DarlingServer::Thread::_dispose() {
	threadLog.debug() << *this << ": dispose thread context" << threadLog.endLog;
	_selfReference = nullptr;
};

void DarlingServer::Thread::_scheduleRelease() {
	// schedule the duct-taped thread to be released
	// dtape_thread_release needs a microthread context, so we call it within a kernel microthread
	threadLog.debug() << *this << ": scheduling release" << threadLog.endLog;
	kernelAsync([self = shared_from_this()]() {
		if (self->_s2cPerformSempahore) {
			dtape_semaphore_destroy(self->_s2cPerformSempahore);
			self->_s2cPerformSempahore = nullptr;
		}
		if (self->_s2cReplySempahore) {
			dtape_semaphore_destroy(self->_s2cReplySempahore);
			self->_s2cReplySempahore = nullptr;
		}
		if (self->_s2cInterruptEnterSemaphore) {
			dtape_semaphore_destroy(self->_s2cInterruptEnterSemaphore);
			self->_s2cInterruptEnterSemaphore = nullptr;
		}
		if (self->_s2cInterruptExitSemaphore) {
			dtape_semaphore_destroy(self->_s2cInterruptExitSemaphore);
			self->_s2cInterruptExitSemaphore = nullptr;
		}
		dtape_thread_release(self->_dtapeThread);
		self->_dtapeThread = nullptr;
	});
};

bool DarlingServer::Thread::_handleInterruptEnterForCurrentThread() {
	// A0-ARCH stage 3: interrupt as CANCELLATION. The old flow resumed the interrupted
	// context synchronously on a borrowed stack (jumpToResume) and detoured its syscall
	// return back here (_syscallReturnHereDuringInterrupt) -- the #114 stack-lifetime
	// disease. Now: sigexc_enter finalizes the interrupted wait as THREAD_INTERRUPTED,
	// which fires a typed Xnu wake; we ARM the frame and simply return. The enter fiber's
	// doneWorking restores the frame's saved context into the live slots and reparks
	// ("repark-interrupt-cancel"), and the pending wake dispatches the cancelled call
	// through the NORMAL path -- its own fiber, its own stack. Our reply is deferred until
	// that call's reply is stashed on the frame (pushCallReply sends it), which is the
	// same guest-visible ordering the synchronous unwind provided. A raw-parked (non-TH_WAIT)
	// interrupted context fires no wake here and just stays parked after the restore; its
	// GENUINE wake eventually resumes it and the stash sends our reply then -- also exactly
	// the old timing.
	//
	// perf#25a A0 (Part 3f): pin `self` -- sigexc_enter takes thread_lock (a dtape mutex
	// whose contention raw-suspends this fiber, possibly migrating OS threads), so
	// currentThreadVar must be re-asserted after it and never re-read.
	std::shared_ptr<Thread> self = currentThreadVar;

	{
		std::unique_lock lock(self->_rwlock);

		// A0-ARCH stage 1c: doWork pushes an InterruptContext for every InterruptEnter
		// dispatch, so an empty stack here means a stray interrupt_exit popped it out from
		// under us (RPC-stream desync). Bail out gracefully: reply 0, survive loudly.
		if (self->_interrupts.empty()) {
			microthreadLog.error() << self->_tid << "(" << self->_nstid << "): interrupt_enter with EMPTY interrupt stack (desync); ignoring" << microthreadLog.endLog;
			return true;
		}

		if (self->_pendingSavedReply) {
			if (self->_interrupts.top().savedReply) {
				throw std::runtime_error("Pending saved reply would overwrite saved reply");
			}

			self->_interrupts.top().savedReply = std::move(*self->_pendingSavedReply);
			self->_pendingSavedReply = std::nullopt;
			// A0 RPC TRACE: pendingSaved -> interruptTop promotion at interrupt_enter; the reply is now on
			// the interrupt stack and will flush at interrupt_exit (FLUSH-SAVED). Not sent here.
			DarlingServer::__rpctrace("PENDINGSAVED->INTERRUPTTOP htid=%d nstid=%lld", self->id(), (long long)self->nsid());
		}
	}

	// Abort the interrupted wait (if any). clear_wait_internal -> thread_go -> thread_unblock
	// writes wait_result = THREAD_INTERRUPTED and fires the typed Xnu wake -- which we KEEP
	// (it is the dispatch vehicle for the cancellation unwind; the old flow dropped it
	// because it resumed synchronously). A pending Raw wake likewise survives untouched.
	dtape_thread_sigexc_enter(self->_dtapeThread);
	currentThreadVar = self; // may have suspended+migrated inside (thread_lock contention)

	// Push the fresh signal-handling user state. Note the order vs the old flow: this now
	// runs BEFORE the interrupted call's unwind (which happens after we return) instead of
	// after it. The unwind paths (wait-abort error paths replying to the guest) do not read
	// the user-state stack, so the inversion is benign -- and the guest still only observes
	// enter2's effect after our reply, which is ordered after the stashed unwind reply.
	dtape_thread_sigexc_enter2(self->_dtapeThread);
	currentThreadVar = self;

	{
		std::unique_lock lock(self->_rwlock);

		if (self->_interrupts.empty()) {
			// A0-ARCH stage 1c: a stray interrupt_exit popped our frame while we were
			// suspended in sigexc_enter (desync); nothing to arm, survive loudly.
			microthreadLog.error() << self->_tid << "(" << self->_nstid << "): interrupt frame popped mid-interrupt_enter (desync); nothing to cancel" << microthreadLog.endLog;
			return true;
		}

		auto& frame = self->_interrupts.top();
		bool hasSavedContext = frame.savedStack.isValid() || frame.savedContinuation;

		if (frame.interruptedCall || hasSavedContext) {
			// something is in flight: arm the restore (doneWorking reparks the saved context)
			frame.cancellationArmed = true;
			self->_mstateEventLocked(StateEvent::Note, "interrupt-cancel-armed", 0, self->_interrupts.size());
		}

		if (frame.interruptedCall) {
			// our reply is owed at the moment the cancelled call's reply is stashed
			frame.replyOwed = true;
			frame.enterCall = self->_activeCall;
			return false;
		}
	}

	// nothing in-flight to cancel (fresh interrupt, e.g. the S2C signal path): reply now
	return true;
};
