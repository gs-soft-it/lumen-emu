// Lumen modification, 2026-10-02: native context architecture is explicit.
#ifndef KYTY_COMMON_HOST_EXCEPTION_H_
#define KYTY_COMMON_HOST_EXCEPTION_H_

#include "common/common.h"

namespace Common::HostException {

enum class ExceptionType { Unknown, AccessViolation, IllegalInstruction };

enum class AccessViolationType { Unknown, Read, Write, Execute };

// This record describes a native host signal/exception context. FEX's guest
// register state must come from the translator, never from ARM host registers.
enum class ContextArchitecture { Unknown, NativeX86_64, NativeArm64 };

struct Arm64Context {
	uint64_t x[31] {};
	uint64_t sp {};
	uint64_t pc {};
	uint32_t pstate {};
	uint32_t esr {};
	bool esr_valid {};
};

struct ExceptionInfo {
	ContextArchitecture context_architecture = ContextArchitecture::Unknown;
	Arm64Context        arm64 {};
	ExceptionType       type                   = ExceptionType::Unknown;
	AccessViolationType access_violation_type  = AccessViolationType::Unknown;
	uint64_t            access_violation_vaddr = 0;
	uint64_t            exception_address      = 0;
	// These fields are valid only for NativeX86_64.
	uint64_t            rax                    = 0;
	uint64_t            rbx                    = 0;
	uint64_t            rcx                    = 0;
	uint64_t            rdx                    = 0;
	uint64_t            rsi                    = 0;
	uint64_t            rdi                    = 0;
	uint64_t            rbp                    = 0;
	uint64_t            rsp                    = 0;
	uint64_t            r8                     = 0;
	uint64_t            r9                     = 0;
	uint64_t            r10                    = 0;
	uint64_t            r11                    = 0;
	uint64_t            r12                    = 0;
	uint64_t            r13                    = 0;
	uint64_t            r14                    = 0;
	uint64_t            r15                    = 0;
	uint32_t            native_code            = 0;
	// Platform-specific mutable context, valid only for the duration of the handler call.
	void* native_context = nullptr;
};

using Handler = bool (*)(const ExceptionInfo&);

bool InstallHandler(Handler handler);

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
bool InitializeThreadSignalStack();
#endif

} // namespace Common::HostException

#endif /* KYTY_COMMON_HOST_EXCEPTION_H_ */
