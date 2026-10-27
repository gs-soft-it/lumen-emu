#include "common/hostException.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#if defined(LUMEN_PS5_DIRECT_FEX)
#include "../../../direct_fex/DirectFexRuntime.h"
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
#include "common/arm64MemoryAccess.h"
#include <cstring>
#endif
#endif
#if defined(__linux__) && defined(__aarch64__)
#include <asm/sigcontext.h>
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h> // IWYU pragma: keep
#else
#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <initializer_list>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <ucontext.h> // IWYU pragma: keep
#endif
#endif

// IWYU pragma: no_include <errhandlingapi.h>
// IWYU pragma: no_include <excpt.h>
// IWYU pragma: no_include <minwinbase.h>
// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <wtypes.h>

namespace Common::HostException {

static std::atomic<const TranslatedMemoryProvider*> g_translation_provider {nullptr};
static_assert(decltype(g_translation_provider)::is_always_lock_free);
void SetTranslatedMemoryProvider(const TranslatedMemoryProvider* provider) noexcept {
    g_translation_provider.store(provider, std::memory_order_release);
}

#if !defined(__APPLE__)

static std::atomic<Handler> g_handler {nullptr};
static std::atomic_uint32_t g_install_state {0};

static_assert(decltype(g_handler)::is_always_lock_free);
static_assert(decltype(g_install_state)::is_always_lock_free);
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX

// macOS uses the same POSIX platform setting and needs a signal stack too.
class ThreadSignalStack {
public:
	ThreadSignalStack() {
		const auto page_size = static_cast<size_t>(::getpagesize());
		const auto stack_size =
		    (std::max<size_t>(64 * 1024, MINSIGSTKSZ) + page_size - 1) & ~(page_size - 1);
		if (::posix_memalign(&m_memory, page_size, stack_size) != 0) {
			return;
		}

		stack_t stack {};
		stack.ss_sp   = m_memory;
		stack.ss_size = stack_size;
		if (::sigaltstack(&stack, &m_previous) != 0) {
			std::free(m_memory);
			m_memory = nullptr;
		}
	}

	~ThreadSignalStack() {
		if (m_memory != nullptr && ::sigaltstack(&m_previous, nullptr) == 0) {
			std::free(m_memory);
		}
	}

	[[nodiscard]] bool IsInitialized() const { return m_memory != nullptr; }

	KYTY_CLASS_NO_COPY(ThreadSignalStack)

private:
	void*   m_memory = nullptr;
	stack_t m_previous {};
};

bool InitializeThreadSignalStack() {
	// Keep fault handling off guest stacks, which GPU tracking can make read-only.
	thread_local ThreadSignalStack signal_stack;
	return signal_stack.IsInitialized();
}

#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

static LONG WINAPI ExceptionFilter(PEXCEPTION_POINTERS exception) noexcept {
	auto* exception_record = exception->ExceptionRecord;

	if (exception_record->ExceptionCode == DBG_PRINTEXCEPTION_C ||
	    exception_record->ExceptionCode == DBG_PRINTEXCEPTION_WIDE_C) {
		return EXCEPTION_CONTINUE_SEARCH;
	}

	if (exception_record->ExceptionCode == 0x406D1388) {
		// Set a thread name.
		return EXCEPTION_CONTINUE_EXECUTION;
	}

	ExceptionInfo info {};
	info.exception_address = reinterpret_cast<uint64_t>(exception_record->ExceptionAddress);
	info.native_code       = exception_record->ExceptionCode;
	info.native_context    = exception->ContextRecord;

	if (exception_record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
		info.type = ExceptionType::AccessViolation;
		switch (exception_record->ExceptionInformation[0]) {
			case 0: info.access_violation_type = AccessViolationType::Read; break;
			case 1: info.access_violation_type = AccessViolationType::Write; break;
			case 8: info.access_violation_type = AccessViolationType::Execute; break;
			default: info.access_violation_type = AccessViolationType::Unknown; break;
		}
		info.access_violation_vaddr = exception_record->ExceptionInformation[1];
	} else if (exception_record->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) {
		info.type = ExceptionType::IllegalInstruction;
	} else {
		return EXCEPTION_CONTINUE_SEARCH;
	}

	info.rax = exception->ContextRecord->Rax;
	info.rbx = exception->ContextRecord->Rbx;
	info.rcx = exception->ContextRecord->Rcx;
	info.rdx = exception->ContextRecord->Rdx;
	info.rsi = exception->ContextRecord->Rsi;
	info.rdi = exception->ContextRecord->Rdi;
	info.rbp = exception->ContextRecord->Rbp;
	info.rsp = exception->ContextRecord->Rsp;
	info.r8  = exception->ContextRecord->R8;
	info.r9  = exception->ContextRecord->R9;
	info.r10 = exception->ContextRecord->R10;
	info.r11 = exception->ContextRecord->R11;
	info.r12 = exception->ContextRecord->R12;
	info.r13 = exception->ContextRecord->R13;
	info.r14 = exception->ContextRecord->R14;
	info.r15 = exception->ContextRecord->R15;

	const auto handler = g_handler.load(std::memory_order_acquire);
	if (handler != nullptr && handler(info)) {
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

#elif defined(__APPLE__)

static std::atomic<Handler> g_handler {nullptr};
static std::atomic_uint32_t g_install_state {0};

static_assert(decltype(g_handler)::is_always_lock_free);
static_assert(decltype(g_install_state)::is_always_lock_free);

#if defined(__aarch64__) || defined(__arm64__)
static AccessViolationType DecodeAccess(uint64_t esr) {
	const auto exception_class = (esr >> 26U) & 0x3fU;
	if (exception_class == 0x20U || exception_class == 0x21U)
		return AccessViolationType::Execute;
	if (exception_class == 0x24U || exception_class == 0x25U)
		return (esr & 0x40U) != 0 ? AccessViolationType::Write : AccessViolationType::Read;
	return AccessViolationType::Unknown;
}

#if defined(LUMEN_PS5_DIRECT_FEX)
static bool ResolveTranslatedOperand(void* opaque, uint64_t address, size_t size,
                                     Arm64MemoryAccess::Access access, void** host) noexcept {
    const auto& provider = *static_cast<const TranslatedMemoryProvider*>(opaque);
    const bool read = access != Arm64MemoryAccess::Access::Write;
    const bool write = access != Arm64MemoryAccess::Access::Read;
    if ((read && !provider.query_guest(address, size, false)) ||
        (write && !provider.query_guest(address, size, true))) return false;
    uint64_t native = 0;
    if (!provider.translate_guest(address, size, &native) || native == address) return false;
    // Resolve only pages actually closed by GPU watches; a readable alias must
    // not initiate a readback or require an already-running GPU worker.
    for (auto cursor = address; cursor < address + size;) {
        const auto boundary = (cursor & ~uint64_t{0x3fff}) + 0x4000;
        const auto length = std::min(boundary, address + size) - cursor;
        if (!provider.query_host(cursor, length, write)) {
            if (!provider.resolve_watch(cursor, write) ||
                !provider.query_host(cursor, length, write)) return false;
        }
        cursor = boundary;
    }
    if ((read && !provider.query_guest(address, size, false)) ||
        (write && !provider.query_guest(address, size, true))) return false;
    *host = reinterpret_cast<void*>(native);
    return true;
}

static bool EmulateTranslatedAccess(uint64_t fault, ucontext_t* uc,
                                    const TranslatedMemoryProvider* provider) noexcept {
	auto* mc = uc->uc_mcontext;
	if (DecodeAccess(mc->__es.__esr) == AccessViolationType::Execute) return false;
	Arm64MemoryAccess::State state;
	for (unsigned i = 0; i < 29; ++i) state.x[i] = mc->__ss.__x[i];
	state.x[29] = mc->__ss.__fp; state.x[30] = mc->__ss.__lr; state.x[31] = mc->__ss.__sp;
	static_assert(sizeof(state.v) == sizeof(mc->__ns.__v));
	std::memcpy(state.v.data(), mc->__ns.__v, sizeof(mc->__ns.__v));
	uint32_t instruction = 0;
	std::memcpy(&instruction, reinterpret_cast<const void*>(mc->__ss.__pc), sizeof(instruction));
	// A GPU worker may install a watch after the permission preflight. Permit
	// the protected alias access to fault through the ordinary inverse-mapped
	// handler and retry, instead of terminating on a blocked nested SIGBUS.
	// This is local to executing the operand; unrelated handlers retain their
	// normal signal masks and no native VM lock is held across GPU readback.
	struct OperandSignalMask {
		sigset_t previous {};
		bool active = false;
		OperandSignalMask() {
			sigset_t faults;
			sigemptyset(&faults); sigaddset(&faults, SIGSEGV); sigaddset(&faults, SIGBUS);
			active = sigprocmask(SIG_UNBLOCK, &faults, &previous) == 0;
		}
		~OperandSignalMask() { if (active) sigprocmask(SIG_SETMASK, &previous, nullptr); }
	} mask;
	if (!mask.active) return false;
	if (!Arm64MemoryAccess::Execute(instruction, fault, state,
	                               ResolveTranslatedOperand, const_cast<TranslatedMemoryProvider*>(provider))) return false;
	for (unsigned i = 0; i < 29; ++i) mc->__ss.__x[i] = state.x[i];
	mc->__ss.__fp = state.x[29]; mc->__ss.__lr = state.x[30]; mc->__ss.__sp = state.x[31];
	std::memcpy(mc->__ns.__v, state.v.data(), sizeof(mc->__ns.__v));
	mc->__ss.__pc += sizeof(instruction);
	return true;
}

static void ReportTranslatedFault(uint64_t pc, uint64_t address, bool readable_pc) noexcept {
	// Bounded diagnostic without allocation, formatting libraries or stdio locks.
	char message[] = "PS5_GUEST_VA_ACCESS_REJECTED pc=0x0000000000000000 address=0x0000000000000000 opcode=0x00000000\n";
	constexpr char hex[] = "0123456789abcdef";
	uint32_t opcode = 0;
	if (readable_pc) std::memcpy(&opcode, reinterpret_cast<const void*>(pc), sizeof(opcode));
	const auto put = [&](size_t offset, uint64_t value, size_t count) {
		for (size_t index = 0; index < count; ++index)
			message[offset + count - index - 1] = hex[(value >> (index * 4)) & 15];
	};
	put(34, pc, 16); put(61, address, 16); put(87, opcode, 8);
	(void)::write(STDERR_FILENO, message, sizeof(message)-1);
}
#endif
#else
// Translate the x86-64 page-fault error code (mcontext __es.__err) into an access type.
// bit 1 (0x2) = write, bit 4 (0x10) = instruction fetch, otherwise a read.
static AccessViolationType DecodeAccess(uint64_t err) {
	if ((err & 0x10u) != 0) {
		return AccessViolationType::Execute;
	}
	if ((err & 0x2u) != 0) {
		return AccessViolationType::Write;
	}
	return AccessViolationType::Read;
}
#endif

// POSIX signal handler that mirrors the Windows vectored handler: build an ExceptionInfo
// from the mcontext and dispatch. A resolved fault (handler returns true) simply returns,
// re-executing the faulting instruction against the now-fixed protection. An unresolved
// fault restores the default disposition so the retry terminates the process.
static void SignalHandler(int sig, siginfo_t* si, void* uctx) {
	struct PreserveErrno {
		int value = errno;
		~PreserveErrno() { errno = value; }
	} preserve_errno;
#if defined(LUMEN_PS5_DIRECT_FEX) && (defined(__aarch64__) || defined(__arm64__))
	// Only immutable, inaccessible Darwin regions are eligible. There cannot be
	// a successful untranslated access to unrelated readable host memory here.
	const auto* translation = g_translation_provider.load(std::memory_order_acquire);
	if ((sig == SIGSEGV || sig == SIGBUS) && translation != nullptr &&
	    translation->is_nonidentity(reinterpret_cast<uint64_t>(si->si_addr), 1)) {
		auto* context = static_cast<ucontext_t*>(uctx);
		if (EmulateTranslatedAccess(reinterpret_cast<uint64_t>(si->si_addr), context, translation)) return;
		const auto pc = context->uc_mcontext->__ss.__pc;
		ReportTranslatedFault(pc, reinterpret_cast<uint64_t>(si->si_addr),
		    DecodeAccess(context->uc_mcontext->__es.__esr) != AccessViolationType::Execute &&
		    !translation->is_nonidentity(pc, sizeof(uint32_t)));
		struct sigaction dfl {};
		dfl.sa_handler = SIG_DFL;
		sigemptyset(&dfl.sa_mask);
		sigaction(sig, &dfl, nullptr);
		return; // unsupported/denied accesses stay faults, never successful reads
	}
#endif
#if defined(LUMEN_PS5_DIRECT_FEX)
	if ((sig == SIGSEGV || sig == SIGBUS) &&
	    Lumen::Ps5::DirectFex::HandleHostSignal(sig, si, uctx)) {
		return;
	}
#endif
	auto*       uc = static_cast<ucontext_t*>(uctx);
	const auto* mc = uc->uc_mcontext;
	const auto& ss = mc->__ss;

	ExceptionInfo info {};
#if defined(__aarch64__) || defined(__arm64__)
	info.exception_address = ss.__pc;
#else
	info.exception_address = ss.__rip;
#endif
	info.native_code       = static_cast<uint32_t>(si->si_code);
	info.native_context    = uctx;

	if (sig == SIGILL) {
		info.type = ExceptionType::IllegalInstruction;
	} else {
		info.type                   = ExceptionType::AccessViolation;
#if defined(__aarch64__) || defined(__arm64__)
		info.access_violation_type  = DecodeAccess(mc->__es.__esr);
#else
		info.access_violation_type  = DecodeAccess(mc->__es.__err);
#endif
		info.access_violation_vaddr = reinterpret_cast<uint64_t>(si->si_addr);
#if defined(LUMEN_PS5_DIRECT_FEX) && (defined(__aarch64__) || defined(__arm64__))
		uint64_t guest_address = 0;
		if (translation != nullptr && translation->translate_host(info.access_violation_vaddr, &guest_address)) {
			info.access_violation_vaddr = guest_address;
			const bool write = info.access_violation_type == AccessViolationType::Write;
			if (info.access_violation_type == AccessViolationType::Execute ||
			    !translation->query_guest(guest_address, 1, write)) {
				struct sigaction dfl {};
				dfl.sa_handler = SIG_DFL;
				sigemptyset(&dfl.sa_mask);
				sigaction(sig, &dfl, nullptr);
				return; // semantic revocation is not a recoverable GPU watch
			}
		}
#endif
	}

#if !defined(__aarch64__) && !defined(__arm64__)
	info.rax = ss.__rax;
	info.rbx = ss.__rbx;
	info.rcx = ss.__rcx;
	info.rdx = ss.__rdx;
	info.rsi = ss.__rsi;
	info.rdi = ss.__rdi;
	info.rbp = ss.__rbp;
	info.rsp = ss.__rsp;
	info.r8  = ss.__r8;
	info.r9  = ss.__r9;
	info.r10 = ss.__r10;
	info.r11 = ss.__r11;
	info.r12 = ss.__r12;
	info.r13 = ss.__r13;
	info.r14 = ss.__r14;
	info.r15 = ss.__r15;
#endif

	const auto handler = g_handler.load(std::memory_order_acquire);
	if (handler != nullptr && handler(info)) {
		return; // retry the faulting instruction against the fixed mapping
	}

	// Unresolved: restore the default action so the re-executed instruction terminates.
	struct sigaction dfl {};
	dfl.sa_handler = SIG_DFL;
	sigemptyset(&dfl.sa_mask);
	sigaction(sig, &dfl, nullptr);
}

#else

// x86-64 page-fault error bits.
constexpr uint64_t PAGE_FAULT_ERROR_WRITE       = 0x02;
constexpr uint64_t PAGE_FAULT_ERROR_INSTRUCTION = 0x10;

// Let the kernel handle an unresolved fault on retry.
static void ChainToDefault(int signal_number) noexcept {
	struct sigaction restore {};
	restore.sa_handler = SIG_DFL;
	sigemptyset(&restore.sa_mask);
	restore.sa_flags = 0;
	::sigaction(signal_number, &restore, nullptr);
}

static void SignalHandler(int signal_number, siginfo_t* signal_info, void* native_context) {
#if defined(LUMEN_PS5_DIRECT_FEX)
	// Match the direct Gen4 signal order: translator-internal faults first,
	// then the original engine's GPU page-watch/guest exception dispatcher.
	if ((signal_number == SIGSEGV || signal_number == SIGBUS) &&
	    Lumen::Ps5::DirectFex::HandleHostSignal(signal_number, signal_info, native_context)) {
		return;
	}
#endif
	auto* context = static_cast<ucontext_t*>(native_context);
#if !defined(__aarch64__)
	auto* gregs   = context->uc_mcontext.gregs;
#endif

	ExceptionInfo info {};
#if defined(__aarch64__)
	info.exception_address = context->uc_mcontext.pc;
#else
	info.exception_address = static_cast<uint64_t>(gregs[REG_RIP]);
#endif
	info.native_code       = static_cast<uint32_t>(signal_number);
	info.native_context    = context;

	if (signal_number == SIGSEGV || signal_number == SIGBUS) {
		info.type             = ExceptionType::AccessViolation;
#if defined(__aarch64__)
		// Linux stores ESR in a bounded extension-record chain, as in the
		// Gen4 direct host adapter. Never infer write access from the address.
		const auto* record = context->uc_mcontext.__reserved;
		const auto* end = record + sizeof(context->uc_mcontext.__reserved);
		while (static_cast<size_t>(end - record) >= sizeof(_aarch64_ctx)) {
			const auto* header = reinterpret_cast<const _aarch64_ctx*>(record);
			if (header->magic == 0 && header->size == 0) break;
			if (header->size < sizeof(*header) || header->size > static_cast<size_t>(end - record) ||
			    header->size % alignof(_aarch64_ctx) != 0) break;
			if (header->magic == ESR_MAGIC) {
				if (header->size < sizeof(esr_context)) break;
				const auto esr = reinterpret_cast<const esr_context*>(record)->esr;
				const auto exception_class = (esr >> 26U) & 0x3fU;
				if (exception_class == 0x20 || exception_class == 0x21)
					info.access_violation_type = AccessViolationType::Execute;
				else if (exception_class == 0x24 || exception_class == 0x25)
					info.access_violation_type = (esr & 0x40) != 0
					    ? AccessViolationType::Write : AccessViolationType::Read;
				break;
			}
			record += header->size;
		}
#else
		const auto error_code = static_cast<uint64_t>(gregs[REG_ERR]);
		if ((error_code & PAGE_FAULT_ERROR_INSTRUCTION) != 0) {
			info.access_violation_type = AccessViolationType::Execute;
		} else if ((error_code & PAGE_FAULT_ERROR_WRITE) != 0) {
			info.access_violation_type = AccessViolationType::Write;
		} else {
			info.access_violation_type = AccessViolationType::Read;
		}
#endif
		info.access_violation_vaddr = reinterpret_cast<uint64_t>(signal_info->si_addr);
	} else if (signal_number == SIGILL) {
		info.type = ExceptionType::IllegalInstruction;
	} else {
		ChainToDefault(signal_number);
		return;
	}

#if !defined(__aarch64__)
	info.rax = static_cast<uint64_t>(gregs[REG_RAX]);
	info.rbx = static_cast<uint64_t>(gregs[REG_RBX]);
	info.rcx = static_cast<uint64_t>(gregs[REG_RCX]);
	info.rdx = static_cast<uint64_t>(gregs[REG_RDX]);
	info.rsi = static_cast<uint64_t>(gregs[REG_RSI]);
	info.rdi = static_cast<uint64_t>(gregs[REG_RDI]);
	info.rbp = static_cast<uint64_t>(gregs[REG_RBP]);
	info.rsp = static_cast<uint64_t>(gregs[REG_RSP]);
	info.r8  = static_cast<uint64_t>(gregs[REG_R8]);
	info.r9  = static_cast<uint64_t>(gregs[REG_R9]);
	info.r10 = static_cast<uint64_t>(gregs[REG_R10]);
	info.r11 = static_cast<uint64_t>(gregs[REG_R11]);
	info.r12 = static_cast<uint64_t>(gregs[REG_R12]);
	info.r13 = static_cast<uint64_t>(gregs[REG_R13]);
	info.r14 = static_cast<uint64_t>(gregs[REG_R14]);
	info.r15 = static_cast<uint64_t>(gregs[REG_R15]);
#endif

	const auto handler = g_handler.load(std::memory_order_acquire);
	if (handler != nullptr && handler(info)) {
		return;
	}

	ChainToDefault(signal_number);
}

#endif

bool InstallHandler(Handler handler) {
	if (handler == nullptr) {
		return false;
	}

	uint32_t expected_state = 0;
	if (!g_install_state.compare_exchange_strong(expected_state, 1, std::memory_order_acq_rel)) {
		return expected_state == 2 && g_handler.load(std::memory_order_acquire) == handler;
	}

	g_handler.store(handler, std::memory_order_release);

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	if (AddVectoredExceptionHandler(0, ExceptionFilter) == nullptr) {
		g_handler.store(nullptr, std::memory_order_release);
		g_install_state.store(0, std::memory_order_release);
		printf("AddVectoredExceptionHandler() failed\n");
		return false;
	}
#elif defined(__APPLE__)
	struct sigaction sa {};
	sa.sa_sigaction = SignalHandler;
	sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&sa.sa_mask);
	// The guest signal-dispatch path (KernelRaiseException) interrupts threads with
	// SIGUSR1; block it while a fault is being resolved so a stop-the-world request
	// cannot preempt the handler between the protection fix and the retry.
	sigaddset(&sa.sa_mask, SIGUSR1);
#if defined(LUMEN_PS5_DIRECT_FEX)
	// Cancellation may unwind a thread; defer it while GPU/page locks are owned.
	sigaddset(&sa.sa_mask, SIGUSR2);
#endif

	// macOS raises SIGBUS for protection faults on some paths and SIGSEGV on others;
	// SIGILL covers instructions the host cannot execute (routed to the x64 emulator).
	bool ok = sigaction(SIGSEGV, &sa, nullptr) == 0 && sigaction(SIGBUS, &sa, nullptr) == 0 &&
	          sigaction(SIGILL, &sa, nullptr) == 0;
	if (!ok) {
		g_handler.store(nullptr, std::memory_order_release);
		g_install_state.store(0, std::memory_order_release);
		printf("sigaction() failed to install the host fault handler\n");
		return false;
	}
#else
	struct sigaction action {};
	action.sa_sigaction = SignalHandler;
	sigemptyset(&action.sa_mask);
#if defined(__ANDROID__) && defined(LUMEN_PS5_DIRECT_FEX)
	// A GPU fault may own cache/page locks while completing readback. Defer
	// asynchronous guest cancellation until the repaired instruction resumes.
	sigaddset(&action.sa_mask, SIGRTMAX);
#endif
	action.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;

	for (const int signal_number: {SIGSEGV, SIGBUS, SIGILL}) {
		if (::sigaction(signal_number, &action, nullptr) != 0) {
			g_handler.store(nullptr, std::memory_order_release);
			g_install_state.store(0, std::memory_order_release);
			printf("sigaction(%d) failed\n", signal_number);
			return false;
		}
	}
#endif

	g_install_state.store(2, std::memory_order_release);
	return true;
}

} // namespace Common::HostException
