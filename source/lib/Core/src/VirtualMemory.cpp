#include "Lumen/Core/VirtualMemory.h"
#include "Lumen/Core/MemoryMetadataScope.h"
#include "../../../cpu/runtime/MemoryHandoff.h"
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
#include "Lumen/Core/ProcMapsReader.h"
#include "Lumen/Core/VirtualMemoryImmutableSnapshot.h"
#endif

#include "Lumen/Sys/SysVirtual.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
#include <android/log.h>
#include <fcntl.h>
#if defined(__aarch64__)
#include <asm/hwcap.h>
#endif
#include <sys/auxv.h>
#include <sys/mman.h>
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
#include <sys/stat.h>
#endif
#include <sys/uio.h>
#include <unistd.h>
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
extern "C" uint64_t LumenAndroidFexCoreBeginGuestMemoryMutationV1() noexcept
	__attribute__((weak));
extern "C" uint64_t LumenAndroidFexCoreBeginGuestMemoryMutationV2(
	uint32_t kind) noexcept __attribute__((weak));
extern "C" uint64_t LumenAndroidFexCoreBeginGuestMemoryMutationV3(
	uint32_t kind, uint32_t* retryable) noexcept __attribute__((weak));
extern "C" uint32_t LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(
	uint64_t token) noexcept __attribute__((weak));
extern "C" void LumenAndroidFexCoreEndGuestMemoryMutationV1(
	uint64_t token, uint64_t range_base, uint64_t range_size,
	uint32_t committed) noexcept __attribute__((weak));
extern "C" void LumenAndroidFexCoreNotifyFreshGuestCallbackMappingChangeV1(
	uint64_t range_base, uint64_t range_size) noexcept __attribute__((weak));
extern "C" uint32_t LumenAndroidFexCoreRetainGuestDataLeaseV1() noexcept
	__attribute__((weak));
extern "C" uint32_t LumenAndroidFexCoreReleaseGuestDataLeaseV1() noexcept
	__attribute__((weak));
extern "C" uint64_t LumenAndroidFexCoreTryBeginNativeGuestDataLeaseV1() noexcept
	__attribute__((weak));
extern "C" uint64_t LumenAndroidFexCoreTryBeginNativeGuestDataLeaseV2(
	uint32_t* mutation_pending) noexcept __attribute__((weak));
extern "C" uint32_t LumenAndroidFexCoreEndNativeGuestDataLeaseV1(uint64_t token) noexcept
	__attribute__((weak));
extern "C" uint32_t LumenAndroidFexCoreOwnsNativeGuestDataLeaseV1(uint64_t token) noexcept
	__attribute__((weak));
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
#define LUMEN_HAS_EXCEPTIONS
#endif

#ifdef LUMEN_HAS_EXCEPTIONS
#include <windows.h> // IWYU pragma: keep
#endif

// IWYU pragma: no_include <basetsd.h>
// IWYU pragma: no_include <errhandlingapi.h>
// IWYU pragma: no_include <excpt.h>
// IWYU pragma: no_include <minwinbase.h>
// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <wtypes.h>

namespace Lumen::Core {

SystemInfo GetSystemInfo()
{
	SystemInfo ret {};

	sys_get_system_info(&ret);

	return ret;
}

namespace VirtualMemory {

namespace {

std::atomic<uint64_t> g_mutation_generation {0};
std::atomic<GuestBackingOwnerQuery> g_backing_owner_query {};
std::atomic<GuestBackingOwnerMark> g_backing_owner_mark {};
std::atomic<GuestBackingOwnerMarkOwned> g_backing_owner_mark_owned {};
std::atomic<GuestBackingOwnerPresence> g_backing_owner_presence {};
std::atomic<GuestBackingOwnerGpuCopy> g_backing_owner_gpu_copy {};

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
constexpr uint64_t ANDROID_DATA_ADDRESS_MASK = UINT64_C(0x00ffffffffffffff);
uint64_t AndroidHostPageSize();
ManagedRegionState QueryAndroidGuestDataRegion(uint64_t address, uint64_t end, ManagedRegion* region);

struct AndroidGuestDataAdmission
{
	uint32_t leases = 0;
	uint32_t native_leases = 0;
	uint32_t mutation_depth = 0;
	uint32_t mutation_leases = 0;
	uint64_t native_token = 0;
	uint64_t external_mutation_token = 0;
	uint64_t external_native_token = 0;
	uint64_t metadata_scope = 0;
};
thread_local AndroidGuestDataAdmission g_android_guest_data;
enum class AndroidGuestDataLeaseKind : uint8_t { None, HostCall, Native, MutationOwner, NativeOwner };

struct AndroidRangeSealRecord
{
	uint64_t ticket = 0;
	uint64_t begin = 0, end = 0;
	bool valid = false;
	AndroidRangeSealRecord* next_free = nullptr;
};

struct AndroidRangeSealBlock
{
	std::array<AndroidRangeSealRecord, 64> records {};
	AndroidRangeSealBlock* next = nullptr;
};

struct AndroidRangeSealRegistry
{
	std::mutex mutex;
	AndroidRangeSealBlock first;
	AndroidRangeSealRecord* free_records = nullptr;
	uint64_t next_ticket = 1;

	AndroidRangeSealRegistry() { AddFreeRecords(&first); }
	void AddFreeRecords(AndroidRangeSealBlock* block)
	{
		for (auto& record: block->records)
		{
			record.next_free = free_records;
			free_records = &record;
		}
	}
};

AndroidRangeSealRegistry* AndroidRangeSeals()
{
	// Tickets may belong to static service objects. Keep their small private
	// registry alive through those objects' destruction, independent of TU order.
	static auto* const registry = new (std::nothrow) AndroidRangeSealRegistry;
	return registry;
}

void InvalidateAndroidRangeSeals(uint64_t address, uint64_t size)
{
	auto* registry = AndroidRangeSeals();
	if (registry == nullptr) return;
	const long detected = sysconf(_SC_PAGESIZE);
	const uint64_t page = detected == 4096 || detected == 16384 ? static_cast<uint64_t>(detected) : 0;
	const uint64_t begin = address & ANDROID_DATA_ADDRESS_MASK;
	bool unknown = address == 0 || size == 0 || page == 0 || size > ANDROID_DATA_ADDRESS_MASK - begin;
	uint64_t first = 0, end = 0;
	if (!unknown)
	{
		first = begin & ~(page - 1);
		const uint64_t limit = begin + size;
		unknown = limit > UINT64_MAX - (page - 1);
		if (!unknown) end = (limit + page - 1) & ~(page - 1);
	}
	std::lock_guard<std::mutex> lock(registry->mutex);
	for (auto* block = &registry->first; block != nullptr; block = block->next)
		for (auto& record: block->records)
			if (record.ticket != 0 && (unknown || (record.begin < end && first < record.end))) record.valid = false;
}
#endif

void RecordMutation()
{
	auto generation = g_mutation_generation.load(std::memory_order_relaxed);
	while (generation != (std::numeric_limits<uint64_t>::max)() &&
	       !g_mutation_generation.compare_exchange_weak(generation, generation + 1, std::memory_order_release, std::memory_order_relaxed))
	{
	}
}

enum class FexGuestMemoryMutationKind : uint32_t
{
	Exclusive       = 0,
	AdditiveMapping = 1,
};

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
class FexGuestMemoryMutation final
{
public:
	explicit FexGuestMemoryMutation(FexGuestMemoryMutationKind kind)
	{
		if (g_android_guest_data.external_native_token != 0) return;
		const bool begin_v1_present =
		    LumenAndroidFexCoreBeginGuestMemoryMutationV1 != nullptr;
		const bool begin_v2_present =
		    LumenAndroidFexCoreBeginGuestMemoryMutationV2 != nullptr;
		const bool begin_v3_present =
		    LumenAndroidFexCoreBeginGuestMemoryMutationV3 != nullptr;
		const bool end_present =
		    LumenAndroidFexCoreEndGuestMemoryMutationV1 != nullptr;
		const bool hooked =
		    begin_v1_present || begin_v2_present || begin_v3_present || end_present;
		if (!hooked)
		{
			admitted = true;
			return;
		}
		if ((!begin_v1_present && !begin_v2_present && !begin_v3_present) || !end_present) return;
		uint32_t may_retry = 0;
		token = begin_v3_present
		            ? LumenAndroidFexCoreBeginGuestMemoryMutationV3(static_cast<uint32_t>(kind), &may_retry)
		            : begin_v2_present
		            ? LumenAndroidFexCoreBeginGuestMemoryMutationV2(
		                  static_cast<uint32_t>(kind))
		            : LumenAndroidFexCoreBeginGuestMemoryMutationV1();
		admitted = token != 0;
		retryable = !admitted && may_retry == 1 && g_android_guest_data.leases == 0 &&
		    g_android_guest_data.native_leases == 0 && g_android_guest_data.mutation_depth == 0;
		if (admitted)
		{
			if (g_android_guest_data.mutation_depth == UINT32_MAX) std::abort();
			++g_android_guest_data.mutation_depth;
		}
	}

	~FexGuestMemoryMutation()
	{
		if (token != 0)
		{
			EndOwnership();
			LumenAndroidFexCoreEndGuestMemoryMutationV1(token, 0, 0, 0);
		}
	}

	FexGuestMemoryMutation(const FexGuestMemoryMutation&) = delete;
	FexGuestMemoryMutation& operator=(const FexGuestMemoryMutation&) = delete;

	bool IsAdmitted() const { return admitted; }
	bool IsRetryable() const { return retryable; }

	void Commit(uint64_t range_base, uint64_t range_size)
	{
		Commit(range_base, range_size, range_base, range_size);
	}

	void Commit(uint64_t range_base, uint64_t range_size,
	            uint64_t lifetime_base, uint64_t lifetime_size)
	{
		if (token != 0)
		{
			if (LumenAndroidFexCoreNotifyFreshGuestCallbackMappingChangeV1 != nullptr)
				LumenAndroidFexCoreNotifyFreshGuestCallbackMappingChangeV1(lifetime_base, lifetime_size);
			EndOwnership();
			LumenAndroidFexCoreEndGuestMemoryMutationV1(
			    token, range_base, range_size, 1);
			token = 0;
		}
	}

private:
	static void EndOwnership()
	{
		if (g_android_guest_data.mutation_depth == 0 ||
		    g_android_guest_data.mutation_leases != 0) std::abort();
		--g_android_guest_data.mutation_depth;
	}
	uint64_t token = 0;
	bool     admitted = false;
	bool     retryable = false;
};
#else
class FexGuestMemoryMutation final
{
public:
	explicit FexGuestMemoryMutation(FexGuestMemoryMutationKind) {}
	bool IsAdmitted() const { return true; }
	bool IsRetryable() const { return false; }
	void Commit(uint64_t, uint64_t) {}
	void Commit(uint64_t, uint64_t, uint64_t, uint64_t) {}
};
#endif

} // namespace

#ifdef LUMEN_HAS_EXCEPTIONS

struct JmpRax
{
	template <class Handler>
	void SetFunc(Handler func)
	{
		*reinterpret_cast<Handler*>(&code[2]) = func;
	}

	// mov rax, 0x1122334455667788
	// jmp rax
	uint8_t code[16] = {0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xFF, 0xE0};
};

class ExceptionHandlerPrivate
{
public:
#pragma pack(1)

	struct UnwindInfo
	{
		uint8_t Version : 3;
		uint8_t Flags   : 5;
		uint8_t SizeOfProlog;
		uint8_t CountOfCodes;
		uint8_t FrameRegister : 4;
		uint8_t FrameOffset   : 4;
		ULONG   ExceptionHandler;

		ExceptionHandlerPrivate* ExceptionData;
	};

	struct HandlerInfo
	{
		JmpRax           code;
		RUNTIME_FUNCTION function_table = {};
		UnwindInfo       unwind_info    = {};
	};

#pragma pack()

	static EXCEPTION_DISPOSITION Handler(PEXCEPTION_RECORD   exception_record, ULONG64 /*EstablisherFrame*/, PCONTEXT /*ContextRecord*/,
	                                     PDISPATCHER_CONTEXT dispatcher_context)
	{
		ExceptionHandler::ExceptionInfo info {};

		info.exception_address = reinterpret_cast<uint64_t>(exception_record->ExceptionAddress);

		if (exception_record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
		{
			info.type = ExceptionHandler::ExceptionType::AccessViolation;
			switch (exception_record->ExceptionInformation[0])
			{
				case 0: info.access_violation_type = ExceptionHandler::AccessViolationType::Read; break;
				case 1: info.access_violation_type = ExceptionHandler::AccessViolationType::Write; break;
				case 8: info.access_violation_type = ExceptionHandler::AccessViolationType::Execute; break;
				default: info.access_violation_type = ExceptionHandler::AccessViolationType::Unknown; break;
			}
			info.access_violation_vaddr = exception_record->ExceptionInformation[1];
		}

		info.rbp                = dispatcher_context->ContextRecord->Rbp;
		info.exception_win_code = exception_record->ExceptionCode;

		auto* p = *static_cast<ExceptionHandlerPrivate**>(dispatcher_context->HandlerData);
		p->func(&info);

		return ExceptionContinueExecution;
	}

	void InitHandler()
	{
		auto* h           = new (reinterpret_cast<void*>(handler_addr)) HandlerInfo;
		auto* code        = &h->code;
		auto* unwind_info = &h->unwind_info;

		function_table = &h->function_table;

		function_table->BeginAddress = 0;
		function_table->EndAddress   = image_size;
		function_table->UnwindData   = reinterpret_cast<uintptr_t>(unwind_info) - base_address;

		unwind_info->Version          = 1;
		unwind_info->Flags            = UNW_FLAG_EHANDLER;
		unwind_info->SizeOfProlog     = 0;
		unwind_info->CountOfCodes     = 0;
		unwind_info->FrameRegister    = 0;
		unwind_info->FrameOffset      = 0;
		unwind_info->ExceptionHandler = reinterpret_cast<uintptr_t>(code) - base_address;
		unwind_info->ExceptionData    = this;

		code->SetFunc(Handler);

		FlushInstructionCache(reinterpret_cast<uint64_t>(code), sizeof(h->code));
	}

	uint64_t          base_address   = 0;
	uint64_t          handler_addr   = 0;
	uint64_t          image_size     = 0;
	PRUNTIME_FUNCTION function_table = nullptr;

	ExceptionHandler::handler_func_t func = nullptr;
};

#else
class ExceptionHandlerPrivate
{
};
#endif

ExceptionHandler::ExceptionHandler(): m_p(new ExceptionHandlerPrivate) {}

ExceptionHandler::~ExceptionHandler()
{
#ifdef LUMEN_HAS_EXCEPTIONS
	Uninstall();
#endif
	delete m_p;
}

uint64_t ExceptionHandler::GetSize()
{
#ifdef LUMEN_HAS_EXCEPTIONS
	return (sizeof(ExceptionHandlerPrivate::HandlerInfo) & ~(static_cast<uint64_t>(0x1000) - 1)) + 0x1000;
#else
	return 0x1000;
#endif
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static, misc-unused-parameters)
bool ExceptionHandler::Install(uint64_t base_address, uint64_t handler_addr, uint64_t image_size, handler_func_t func)
{
#ifdef LUMEN_HAS_EXCEPTIONS
	if (m_p->function_table == nullptr)
	{
		m_p->base_address = base_address;
		m_p->handler_addr = handler_addr;
		m_p->image_size   = image_size;
		m_p->func         = func;

		m_p->InitHandler();

		if (RtlAddFunctionTable(m_p->function_table, 1, base_address) == FALSE)
		{
			printf("RtlAddFunctionTable() failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
			return false;
		}

		return true;
	}

	return false;
#else
	return true;
#endif
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static, misc-unused-parameters)
bool ExceptionHandler::Uninstall()
{
#ifdef LUMEN_HAS_EXCEPTIONS
	if (m_p->function_table != nullptr)
	{
		if (RtlDeleteFunctionTable(m_p->function_table) == FALSE)
		{
			printf("RtlDeleteFunctionTable() failed: 0x%08" PRIx32 "\n", static_cast<uint32_t>(GetLastError()));
			return false;
		}
		m_p->function_table = nullptr;
		return true;
	}

	return false;
#else
	return true;
#endif
}

void Init()
{
	sys_virtual_init();
}

bool RegisterGuestBackingOwner(GuestBackingOwnerQuery query, GuestBackingOwnerMark mark,
    GuestBackingOwnerPresence presence, GuestBackingOwnerGpuCopy copy, GuestBackingOwnerMarkOwned mark_owned) noexcept
{
	if (query == nullptr || mark == nullptr) return false;
	static std::mutex registration_mutex;
	std::lock_guard lock(registration_mutex);
	const auto current = g_backing_owner_query.load(std::memory_order_acquire);
	if (current != nullptr)
		return current == query && g_backing_owner_mark.load(std::memory_order_acquire) == mark &&
		    g_backing_owner_mark_owned.load(std::memory_order_acquire) == mark_owned &&
		    g_backing_owner_presence.load(std::memory_order_acquire) == presence &&
		    g_backing_owner_gpu_copy.load(std::memory_order_acquire) == copy;
	g_backing_owner_mark.store(mark, std::memory_order_release);
	g_backing_owner_mark_owned.store(mark_owned, std::memory_order_release);
	g_backing_owner_presence.store(presence, std::memory_order_release);
	g_backing_owner_gpu_copy.store(copy, std::memory_order_release);
	g_backing_owner_query.store(query, std::memory_order_release);
	return true;
}


uint64_t GetMutationGeneration()
{
	return g_mutation_generation.load(std::memory_order_acquire);
}

bool TryAcquireCodeReadLease(uint64_t, uint64_t, CodeReadLease* lease)
{
	if (lease != nullptr)
	{
		lease->mapping_identity = 0;
		lease->host_begin = 0;
		lease->byte_count = 0;
		lease->mutation_generation = 0;
	}
	// Only the Apple mapping registry currently carries a proven physical-page
	// ownership protocol. Other platforms fail closed until their VM backend can
	// pin lifetime and exclude writers with its native page geometry.
	return false;
}

void ReleaseCodeReadLease(CodeReadLease* lease)
{
	if (lease != nullptr)
	{
		lease->mapping_identity = 0;
		lease->host_begin = 0;
		lease->byte_count = 0;
		lease->mutation_generation = 0;
	}
}

GuestMemoryMutationOwnerScope::GuestMemoryMutationOwnerScope(uint64_t owned_token)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	auto& admission = g_android_guest_data;
	if (owned_token == 0 || LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1 == nullptr ||
	    admission.leases != 0 || admission.native_token != 0 || admission.external_native_token != 0 || admission.mutation_depth != 0 ||
	    LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(owned_token) != 1) return;
	admission.external_mutation_token = owned_token;
	admission.metadata_scope = Detail::NewMemoryMetadataScope();
	admission.mutation_depth = 1;
	token = owned_token;
	acquiring_thread = reinterpret_cast<uintptr_t>(&admission);
#else
	(void)owned_token;
#endif
}

GuestMemoryMutationOwnerScope::~GuestMemoryMutationOwnerScope()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (token == 0) return;
	auto& admission = g_android_guest_data;
	if (acquiring_thread != reinterpret_cast<uintptr_t>(&admission) ||
	    admission.external_mutation_token != token || admission.mutation_depth != 1 ||
	    admission.leases != 0 || admission.mutation_leases != 0 ||
	    LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1 == nullptr ||
	    LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(token) != 1) std::abort();
	admission.external_mutation_token = 0;
	admission.metadata_scope = 0;
	admission.mutation_depth = 0;
#endif
}

uint64_t GetOwnedGuestMemoryMutationToken() noexcept
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	const auto& admission = g_android_guest_data;
	const uint64_t owned_token = admission.external_mutation_token;
	if (owned_token != 0 && admission.mutation_depth == 1 &&
	    LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1 != nullptr &&
	    LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(owned_token) == 1) return admission.metadata_scope;
#endif
	return 0;
}

GuestDataLease::~GuestDataLease()
{
	ReleaseGuestDataLease(this);
}

GuestNativeMemoryOwnerScope::GuestNativeMemoryOwnerScope(uint64_t native_token)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	auto& admission = g_android_guest_data;
	if (native_token == 0 || admission.leases != 0 || admission.native_token != 0 ||
	    admission.external_native_token != 0 || admission.mutation_depth != 0 ||
	    admission.external_mutation_token != 0 || LumenAndroidFexCoreOwnsNativeGuestDataLeaseV1 == nullptr ||
	    LumenAndroidFexCoreOwnsNativeGuestDataLeaseV1(native_token) != 1) return;
	admission.external_native_token = native_token;
	token = native_token;
	acquiring_thread = reinterpret_cast<uintptr_t>(&admission);
#else
	(void)native_token;
#endif
}

GuestNativeMemoryOwnerScope::~GuestNativeMemoryOwnerScope()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (token == 0) return;
	auto& admission = g_android_guest_data;
	if (acquiring_thread != reinterpret_cast<uintptr_t>(&admission) || admission.external_native_token != token ||
	    admission.leases != 0 || admission.native_leases != 0 || admission.mutation_depth != 0 ||
	    LumenAndroidFexCoreOwnsNativeGuestDataLeaseV1 == nullptr ||
	    LumenAndroidFexCoreOwnsNativeGuestDataLeaseV1(token) != 1) std::abort();
	admission.external_native_token = 0;
#endif
}

bool CurrentNativeClaimCoversGuestRange(uint64_t address, uint64_t bytes) noexcept
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	address &= ANDROID_DATA_ADDRESS_MASK;
	if (g_android_guest_data.external_native_token != 0 &&
	    (LumenAndroidFexCoreOwnsNativeGuestDataLeaseV1 == nullptr ||
	     LumenAndroidFexCoreOwnsNativeGuestDataLeaseV1(g_android_guest_data.external_native_token) != 1)) return false;
#endif
	if (address == 0 || bytes == 0 || address > UINT64_MAX - bytes ||
	    GuestCpu::CurrentMemoryHandoffNativeClaimOwner() == 0) return false;
	const auto end = address + bytes;
	while (address < end)
	{
		GuestBackingSpan span;
		if (!QueryGuestBackingSpan(address, end - address, &span) || span.identity == 0 ||
		    span.address != address || span.size == 0 || span.size > end - address ||
		    span.offset > UINT64_MAX - span.size ||
		    !GuestCpu::NativeClaimCoversPhysical({span.identity, span.offset, span.size})) return false;
		address += span.size;
	}
	return true;
}

namespace {
bool NativeClaimCoverageRequired() noexcept
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (g_android_guest_data.external_native_token != 0) return true;
#endif
	return GuestCpu::CurrentMemoryHandoffNativeClaimOwner() != 0;
}
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
namespace {

bool BeginAndroidGuestDataAdmission(AndroidGuestDataLeaseKind* kind, bool* retryable = nullptr)
{
	if (retryable != nullptr) *retryable = false;
	auto& admission = g_android_guest_data;
	if (admission.leases == UINT32_MAX) return false;
	if (admission.mutation_depth != 0)
	{
		// An adopted native producer must still own the exact translator epoch;
		// a token value or a mapping query alone never grants data admission.
		if (admission.external_mutation_token != 0 &&
		    (LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1 == nullptr ||
		     LumenAndroidFexCoreOwnsExclusiveGuestMemoryMutationV1(admission.external_mutation_token) != 1)) return false;
		if (admission.mutation_leases == UINT32_MAX) return false;
		++admission.mutation_leases;
		*kind = AndroidGuestDataLeaseKind::MutationOwner;
	} else if (admission.external_native_token != 0)
	{
		if (LumenAndroidFexCoreOwnsNativeGuestDataLeaseV1 == nullptr ||
		    LumenAndroidFexCoreOwnsNativeGuestDataLeaseV1(admission.external_native_token) != 1) return false;
		*kind = AndroidGuestDataLeaseKind::NativeOwner;
	} else if (admission.native_token != 0)
	{
		++admission.native_leases;
		*kind = AndroidGuestDataLeaseKind::Native;
	} else if (LumenAndroidFexCoreRetainGuestDataLeaseV1 != nullptr &&
	           LumenAndroidFexCoreReleaseGuestDataLeaseV1 != nullptr &&
	           LumenAndroidFexCoreRetainGuestDataLeaseV1() != 0)
	{
		*kind = AndroidGuestDataLeaseKind::HostCall;
	} else
	{
		// The provider independently rejects native admission from a guest/HLE
		// reader, writer, suspended wait or pending exclusive epoch. A failed
		// retain therefore cannot turn an invalid callback into a native reader.
		if (LumenAndroidFexCoreEndNativeGuestDataLeaseV1 == nullptr) return false;
		uint32_t mutation_pending = 0;
		if (LumenAndroidFexCoreTryBeginNativeGuestDataLeaseV2 != nullptr)
			admission.native_token = LumenAndroidFexCoreTryBeginNativeGuestDataLeaseV2(&mutation_pending);
		else if (LumenAndroidFexCoreTryBeginNativeGuestDataLeaseV1 != nullptr)
			admission.native_token = LumenAndroidFexCoreTryBeginNativeGuestDataLeaseV1();
		if (admission.native_token == 0)
		{
			if (retryable != nullptr) *retryable = mutation_pending == 1;
			return false;
		}
		admission.native_leases = 1;
		*kind = AndroidGuestDataLeaseKind::Native;
	}
	++admission.leases;
	return true;
}

uint32_t AndroidGuestDataRights(Mode required)
{
	const uint32_t mode = static_cast<uint32_t>(required);
	if (mode == 0 || mode > static_cast<uint32_t>(Mode::ExecuteReadWrite)) return 0;
	// x86 has no data-unreadable present page: Write and Execute metadata also
	// permit data reads. Write permission remains exactly its original bit.
	return static_cast<uint32_t>(Mode::Read) | (mode & static_cast<uint32_t>(Mode::Write));
}

void EndAndroidGuestDataAdmission(AndroidGuestDataLeaseKind kind)
{
	auto& admission = g_android_guest_data;
	if (admission.leases == 0) std::abort();
	--admission.leases;
	switch (kind)
	{
		case AndroidGuestDataLeaseKind::HostCall:
			if (LumenAndroidFexCoreReleaseGuestDataLeaseV1 == nullptr ||
			    LumenAndroidFexCoreReleaseGuestDataLeaseV1() == 0) std::abort();
			break;
		case AndroidGuestDataLeaseKind::Native:
			if (admission.native_token == 0 || admission.native_leases == 0) std::abort();
			if (--admission.native_leases == 0)
			{
				if (LumenAndroidFexCoreEndNativeGuestDataLeaseV1 == nullptr ||
				    LumenAndroidFexCoreEndNativeGuestDataLeaseV1(admission.native_token) == 0)
					std::abort();
				admission.native_token = 0;
			}
			break;
		case AndroidGuestDataLeaseKind::MutationOwner:
			if (admission.mutation_depth == 0 || admission.mutation_leases == 0) std::abort();
			--admission.mutation_leases;
			break;
		case AndroidGuestDataLeaseKind::NativeOwner:
			if (admission.external_native_token == 0 || LumenAndroidFexCoreOwnsNativeGuestDataLeaseV1 == nullptr ||
			    LumenAndroidFexCoreOwnsNativeGuestDataLeaseV1(admission.external_native_token) != 1) std::abort();
			break;
		case AndroidGuestDataLeaseKind::None: std::abort();
	}
}

struct AndroidGuestDataAdmissionScope
{
	AndroidGuestDataLeaseKind kind;
	void Release()
	{
		if (kind == AndroidGuestDataLeaseKind::None) return;
		EndAndroidGuestDataAdmission(kind);
		kind = AndroidGuestDataLeaseKind::None;
	}
	~AndroidGuestDataAdmissionScope() { Release(); }
};

bool AndroidGuestDataLogicalAccess(uint64_t address, uint64_t end, Mode required)
{
	constexpr uint64_t guest_page_size = 4096;
	const uint32_t rights = static_cast<uint32_t>(required);
	while (address < end)
	{
		ManagedRegion region {};
		const auto state = QueryAndroidGuestDataRegion(address, end, &region);
		if (state == ManagedRegionState::Reserved) return false;
		if (state == ManagedRegionState::Visible)
		{
			// x86 writable and executable pages are data-readable. Android's
			// translated guest code is host-readable, never native executable.
			const uint32_t accessible = AndroidGuestDataRights(region.mode);
			if (region.begin > address || region.end <= address ||
			    (accessible & rights) != rights)
				return false;
			address = region.end < end ? region.end : end;
		} else
		{
			// Native buffers remain caller-owned. Do not skip a managed guard or
			// reserved hole later in the same physical VMA.
			const uint64_t next_page = (address & ~(guest_page_size - 1)) + guest_page_size;
			address = next_page < end ? next_page : end;
		}
	}
	return true;
}

bool AndroidGuestDataPhysicalAccess(uint64_t address, uint64_t end, Mode required,
	GuestDataLeaseDenialMetadata* denial)
{
	static const bool direct_read = [] {
		const char* value = std::getenv("LUMEN_GUEST_MAPS_DIRECT_READ");
		const bool selected = value != nullptr && std::strcmp(value, "1") == 0;
		__android_log_print(ANDROID_LOG_INFO, "LumenGuestMemory",
		    "LUMEN_GUEST_MAPS_DIRECT_READ_V1 selected=%u fresh_query=1\n", selected ? 1U : 0U);
		return selected;
	}();
	FILE* const maps = direct_read ? nullptr : std::fopen("/proc/self/maps", "r");
	int descriptor = maps != nullptr ? fileno(maps) : -1;
	if (direct_read)
	{
		do { descriptor = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC); }
		while (descriptor < 0 && errno == EINTR);
	}
	struct CloseMaps
	{
		FILE* stream;
		int descriptor;
		~CloseMaps()
		{
			if (stream != nullptr) std::fclose(stream);
			else if (descriptor >= 0) ::close(descriptor);
		}
	} close_maps {maps, descriptor};
	if (descriptor < 0)
	{
		if (denial != nullptr) denial->physical_result = errno;
		return false;
	}
#if defined(PROCMAP_QUERY)
	// Cache only that the ioctl transport is unavailable. Every fallback still
	// opens and scans current maps under the mapping lease; neither VMA rights
	// nor a successful query survive this call. Repeating a policy-denied ioctl
	// for every borrow can itself flood Android's SELinux audit path.
	static std::atomic<bool> query_unavailable {false};
	if (!query_unavailable.load(std::memory_order_relaxed))
	{
		uint32_t observed = 0;
		int query_error = 0;
		const auto query = ProcMaps::QueryRange(descriptor, address, end,
		    static_cast<uint32_t>(required), &observed, &query_error);
		if (query != ProcMaps::RangeQueryResult::Unsupported)
		{
			if (denial != nullptr)
			{
				denial->physical_observed = query_error == 0;
				denial->physical_protection = observed;
				denial->physical_result = query_error;
			}
			return query == ProcMaps::RangeQueryResult::Allowed;
		}
		if (!query_unavailable.exchange(true, std::memory_order_relaxed))
			__android_log_print(ANDROID_LOG_INFO, "LumenGuestMemory",
			    "LUMEN_PROCMAP_QUERY_V1 available=0 errno=%d fallback=fresh-text\n", query_error);
	}
#endif
	ProcMaps::Reader reader = direct_read ? ProcMaps::Reader(descriptor) : ProcMaps::Reader(maps);
	ProcMaps::Record mapping;
	const uint32_t rights = static_cast<uint32_t>(required);
	while (address < end && reader.Next(&mapping, address))
	{
		if (mapping.end <= address) continue;
		uint32_t observed = 0;
		if (mapping.readable) observed |= static_cast<uint32_t>(Mode::Read);
		if (mapping.writable) observed |= static_cast<uint32_t>(Mode::Write);
		if (denial != nullptr)
		{
			denial->physical_observed = true;
			denial->physical_protection = observed;
		}
		if (mapping.begin > address || (observed & rights) != rights) break;
		address = mapping.end < end ? mapping.end : end;
	}
	return address == end;
}

bool AndroidGuestDataKernelCopyEnabled()
{
#if defined(__aarch64__) && defined(HWCAP2_MTE)
	// A zero logical tag can still mismatch a PROT_MTE allocation tag. The
	// remote-page kernel copy cannot preserve that native source-access check,
	// so retain the ordinary lease route on every MTE-capable host. Hardware
	// capability is stable; no thread TCF state or per-range permissions cache.
	static const bool enabled = [] {
		const int saved_errno = errno;
		errno = 0;
		const unsigned long capabilities = getauxval(AT_HWCAP2);
		const bool known = errno == 0;
		errno = saved_errno;
		return known && (capabilities & HWCAP2_MTE) == 0;
	}();
	return enabled;
#else
	// Without the active Android/AArch64 capability ABI, preserve native reads.
	return false;
#endif
}

bool AndroidGuestDataKernelReadPhysicalAccess(uint64_t address, uint64_t bytes,
	GuestDataLeaseDenialMetadata* denial)
{
	// Permission proof only: keep any bytes, including a partial prefix,
	// private. The caller still completes data handoff before exposing its
	// raw read, and retains the same mapping admission until that read ends.
	std::array<uint8_t, 256> scratch;
	const iovec local {scratch.data(), static_cast<size_t>(bytes)};
	const iovec remote {reinterpret_cast<void*>(address), static_cast<size_t>(bytes)};
	const ssize_t copied = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
	if (denial != nullptr)
	{
		denial->physical_observed = copied == static_cast<ssize_t>(bytes);
		denial->physical_protection = copied == static_cast<ssize_t>(bytes) ? static_cast<uint32_t>(Mode::Read) : 0;
		denial->physical_result = copied < 0 ? errno : 0;
	}
	return copied == static_cast<ssize_t>(bytes);
}

bool DenyAndroidGuestDataCopy(const char* operation, const char* stage, uint64_t address, uint64_t bytes,
	ssize_t syscall_count = -1, int syscall_error = 0,
	const GuestDataLeaseDenialMetadata* lease_denial = nullptr)
{
	// Bounded failure diagnostics remain available without enabling expensive
	// rendering traces. Successful copies do not format text or touch the counter.
	thread_local uint32_t reports = 0;
	if (reports >= 64) return false;
	const int saved_errno = errno;
	++reports;
	char message[768] {};
	std::snprintf(message, sizeof(message),
		    "LUMEN_GUEST_DATA_%s_DENIED_V1 stage=%s address=0x%" PRIx64
		    " bytes=%" PRIu64 " syscall_count=%zd errno=%d lease_reason=%s"
		    " physical_observed=%u physical_result=%d physical_protection=%u"
		    " admission_leases=%u native_leases=%u mutation_depth=%u report=%u\n",
		    operation, stage, address, bytes, syscall_count, syscall_error,
		    lease_denial != nullptr ? GuestDataLeaseDenialReasonName(lease_denial->reason) : "none",
		    lease_denial != nullptr && lease_denial->physical_observed ? 1U : 0U,
		    lease_denial != nullptr ? lease_denial->physical_result : 0,
		    lease_denial != nullptr ? lease_denial->physical_protection : 0,
		    g_android_guest_data.leases, g_android_guest_data.native_leases,
		    g_android_guest_data.mutation_depth, reports);
	__android_log_write(ANDROID_LOG_WARN, "LumenGuestMemory", message);
	const char* trace = std::getenv("LUMEN_STARTUP_TRACE");
	if (trace != nullptr && std::strcmp(trace, "1") == 0) std::fputs(message, stderr);
	errno = saved_errno;
	return false;
}

bool DenyAndroidGuestDataRead(const char* stage, uint64_t address, uint64_t bytes,
	ssize_t syscall_count = -1, int syscall_error = 0,
	const GuestDataLeaseDenialMetadata* lease_denial = nullptr)
{
	return DenyAndroidGuestDataCopy("READ", stage, address, bytes, syscall_count, syscall_error, lease_denial);
}

bool DenyAndroidGuestDataWrite(const char* stage, uint64_t address, uint64_t bytes,
	ssize_t syscall_count = -1, int syscall_error = 0,
	const GuestDataLeaseDenialMetadata* lease_denial = nullptr)
{
	return DenyAndroidGuestDataCopy("WRITE", stage, address, bytes, syscall_count, syscall_error, lease_denial);
}

} // namespace

bool TryReadGuestData(uint64_t address, void* destination, uint64_t bytes,
	GuestDataLeaseDenialMetadata* denial)
{
	constexpr size_t copied_read_limit = 256;
	if (denial != nullptr) *denial = {};
	GuestDataLeaseDenialMetadata observation {};
	observation.address = address;
	observation.bytes = bytes;
	observation.required = Mode::Read;
	observation.acquisition_thread = reinterpret_cast<uintptr_t>(&g_android_guest_data);
	const auto deny = [&](GuestDataLeaseDenialReason reason, const char* stage,
	                      ssize_t count = -1, int error = 0) {
		observation.reason = reason;
		observation.generation = GetMutationGeneration();
		if (denial != nullptr) *denial = observation;
		return DenyAndroidGuestDataRead(stage, address, bytes, count, error, &observation);
	};
	if (destination == nullptr) return deny(GuestDataLeaseDenialReason::LeaseOrRangeInvalid, "range");
	if (NativeClaimCoverageRequired() && !CurrentNativeClaimCoversGuestRange(address, bytes))
		return deny(GuestDataLeaseDenialReason::DataHandoffDenied, "native-claim");
	if (bytes > copied_read_limit || (address & ~ANDROID_DATA_ADDRESS_MASK) != 0 ||
	    !AndroidGuestDataKernelCopyEnabled())
	{
		// Keep tagged pointers and MTE hosts on the ordinary lease path; a
		// kernel copy would bypass the caller's native source tag check.
		GuestDataLease lease;
		if (!TryAcquireGuestDataLease(address, bytes, Mode::Read, &lease, true, &observation))
			return deny(observation.reason, "lease");
		std::memcpy(destination, reinterpret_cast<const void*>(lease.HostBegin()),
		            static_cast<size_t>(bytes));
		return true;
	}
	if (address == 0 || bytes == 0 || bytes > ANDROID_DATA_ADDRESS_MASK - address)
		return deny(GuestDataLeaseDenialReason::LeaseOrRangeInvalid, "range");

	AndroidGuestDataLeaseKind kind = AndroidGuestDataLeaseKind::None;
	if (!BeginAndroidGuestDataAdmission(&kind, &observation.retryable))
		return deny(observation.retryable ? GuestDataLeaseDenialReason::GateWriterPending
		                                 : GuestDataLeaseDenialReason::MappingLeaseCapabilityUnavailable,
		            "admission");
	AndroidGuestDataAdmissionScope admission {kind};
	if (!AndroidGuestDataLogicalAccess(address, address + bytes, Mode::Read))
	{
		admission.Release();
		return deny(GuestDataLeaseDenialReason::LogicalRegistryDenied, "logical");
	}
	if (!GuestCpu::PrepareCpuMemoryAccess(address, bytes, GuestCpu::MemoryAccessIntent::Read))
	{
		admission.Release();
		return deny(GuestDataLeaseDenialReason::DataHandoffDenied, "handoff");
	}

	// The kernel verifies the live physical range while Core/FEX admission
	// retains mapping lifetime. A short or denied copy must neither publish
	// partial bytes nor fall back to a raw access. This retains no permission
	// proof across calls, including changes made outside Core's VM registry.
	std::array<uint8_t, copied_read_limit> scratch;
	const iovec local {scratch.data(), static_cast<size_t>(bytes)};
	const iovec remote {reinterpret_cast<void*>(address), static_cast<size_t>(bytes)};
	const ssize_t copied = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
	if (copied != static_cast<ssize_t>(bytes))
	{
		const int syscall_error = copied < 0 ? errno : 0;
		admission.Release();
		return deny(GuestDataLeaseDenialReason::PhysicalQueryOrRightsDenied, "kernel", copied, syscall_error);
	}
	std::memcpy(destination, scratch.data(), static_cast<size_t>(bytes));
	return true;
}

PrivateGuestDataCopyResult CopyGuestDataToPrivateScratch(uint64_t address, void* scratch, uint64_t bytes,
	GuestDataLeaseDenialMetadata* denial)
{
	if (denial != nullptr) *denial = {};
	GuestDataLeaseDenialMetadata observation {};
	observation.address = address;
	observation.bytes = bytes;
	observation.required = Mode::Read;
	observation.acquisition_thread = reinterpret_cast<uintptr_t>(&g_android_guest_data);
	const auto deny = [&](GuestDataLeaseDenialReason reason, const char* stage,
	                      ssize_t count = -1, int error = 0) {
		observation.reason = reason;
		observation.generation = GetMutationGeneration();
		if (denial != nullptr) *denial = observation;
		DenyAndroidGuestDataRead(stage, address, bytes, count, error, &observation);
		return PrivateGuestDataCopyResult::Denied;
	};
	const uint64_t host_address = address & ANDROID_DATA_ADDRESS_MASK;
	if (scratch == nullptr || address == 0 || host_address == 0 || bytes == 0 ||
	    address > UINT64_MAX - bytes || bytes > ANDROID_DATA_ADDRESS_MASK - host_address)
		return deny(GuestDataLeaseDenialReason::LeaseOrRangeInvalid, "private-range");
	if (NativeClaimCoverageRequired() && !CurrentNativeClaimCoversGuestRange(address, bytes))
		return deny(GuestDataLeaseDenialReason::DataHandoffDenied, "private-native-claim");
	if (bytes > PRIVATE_GUEST_DATA_COPY_LIMIT || (address & ~ANDROID_DATA_ADDRESS_MASK) != 0 ||
	    !AndroidGuestDataKernelCopyEnabled()) return PrivateGuestDataCopyResult::Unsupported;

	AndroidGuestDataLeaseKind kind = AndroidGuestDataLeaseKind::None;
	if (!BeginAndroidGuestDataAdmission(&kind, &observation.retryable))
		return deny(observation.retryable ? GuestDataLeaseDenialReason::GateWriterPending
		                                 : GuestDataLeaseDenialReason::MappingLeaseCapabilityUnavailable,
		            "private-admission");
	AndroidGuestDataAdmissionScope admission {kind};
	if (!AndroidGuestDataLogicalAccess(address, address + bytes, Mode::Read))
	{
		admission.Release();
		return deny(GuestDataLeaseDenialReason::LogicalRegistryDenied, "private-logical");
	}
	if (!GuestCpu::PrepareCpuMemoryAccess(address, bytes, GuestCpu::MemoryAccessIntent::Read))
	{
		admission.Release();
		return deny(GuestDataLeaseDenialReason::DataHandoffDenied, "private-handoff");
	}
	// Unlike TryReadGuestData, this API deliberately exposes incomplete output
	// only to the caller's private scratch. No second allocation/copy is needed,
	// and no kernel error can authorize consumption or a raw-access fallback.
	const iovec local {scratch, static_cast<size_t>(bytes)};
	const iovec remote {reinterpret_cast<void*>(address), static_cast<size_t>(bytes)};
	const ssize_t copied = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
	if (copied != static_cast<ssize_t>(bytes))
	{
		const int syscall_error = copied < 0 ? errno : 0;
		admission.Release();
		return deny(GuestDataLeaseDenialReason::PhysicalQueryOrRightsDenied, "private-kernel", copied, syscall_error);
	}
	return PrivateGuestDataCopyResult::Copied;
}

bool TryWriteGuestData(uint64_t address, const void* source, uint64_t bytes)
{
	constexpr size_t copied_write_limit = 256;
	const uint64_t host_address = address & ANDROID_DATA_ADDRESS_MASK;
	if (source == nullptr || address == 0 || host_address == 0 || bytes == 0 ||
	    address > UINT64_MAX - bytes || bytes > (std::numeric_limits<size_t>::max)() ||
	    bytes > ANDROID_DATA_ADDRESS_MASK - host_address)
		return DenyAndroidGuestDataWrite("range", address, bytes);
	if (NativeClaimCoverageRequired() && !CurrentNativeClaimCoversGuestRange(address, bytes))
		return DenyAndroidGuestDataWrite("native-claim", address, bytes);
	const uint64_t page = AndroidHostPageSize();
	const bool single_page = page != 0 && bytes <= page - (host_address & (page - 1));
	if (bytes > copied_write_limit || (address & ~ANDROID_DATA_ADDRESS_MASK) != 0 ||
	    !single_page || !AndroidGuestDataKernelCopyEnabled())
	{
		GuestDataLease lease;
		GuestDataLeaseDenialMetadata denial;
		if (!TryAcquireGuestDataLease(address, bytes, Mode::Write, &lease, true, &denial))
			return DenyAndroidGuestDataWrite("lease", address, bytes, -1, 0, &denial);
		std::memcpy(reinterpret_cast<void*>(lease.HostBegin()), source, static_cast<size_t>(bytes));
		return true;
	}

	AndroidGuestDataLeaseKind kind = AndroidGuestDataLeaseKind::None;
	if (!BeginAndroidGuestDataAdmission(&kind))
		return DenyAndroidGuestDataWrite("admission", address, bytes);
	AndroidGuestDataAdmissionScope admission {kind};
	if (!AndroidGuestDataLogicalAccess(address, address + bytes, Mode::Write))
	{
		admission.Release();
		return DenyAndroidGuestDataWrite("logical", address, bytes);
	}
	if (!GuestCpu::PrepareCpuMemoryAccess(address, bytes, GuestCpu::MemoryAccessIntent::Write))
	{
		admission.Release();
		return DenyAndroidGuestDataWrite("handoff", address, bytes);
	}

	// Own the complete input before the first guest store. Alignment keeps the
	// source in one stable stack page for both admitted host geometries (4/16K).
	// Linux pins the single remote page with FOLL_WRITE before copying from this
	// valid local page. No remote prefix precedes a permission/pinning failure.
	// This is not an atomic store with respect to concurrent guest readers.
	alignas(copied_write_limit) std::array<uint8_t, copied_write_limit> scratch;
	std::memcpy(scratch.data(), source, static_cast<size_t>(bytes));
	const iovec local {scratch.data(), static_cast<size_t>(bytes)};
	const iovec remote {reinterpret_cast<void*>(address), static_cast<size_t>(bytes)};
	const ssize_t copied = process_vm_writev(getpid(), &local, 1, &remote, 1, 0);
	if (copied == static_cast<ssize_t>(bytes)) return true;
	const int syscall_error = copied < 0 ? errno : 0;
	admission.Release();
	if (copied > 0)
	{
		// A short result violates the bounded-copy assumptions but may already
		// have modified guest bytes. Do not return a recoverable failure, retry,
		// fall back to memcpy, or attempt an unsafe rollback over guest writers.
		__android_log_print(ANDROID_LOG_FATAL, "LumenGuestMemory",
		    "LUMEN_GUEST_DATA_WRITE_PARTIAL_FATAL_V1 address=0x%" PRIx64
		    " bytes=%" PRIu64 " syscall_count=%zd", address, bytes, copied);
		std::abort();
	}
	return DenyAndroidGuestDataWrite("kernel", address, bytes, copied, syscall_error);
}

bool TryAcquireGuestDataLease(uint64_t address, uint64_t bytes, Mode mode,
	GuestDataLease* lease, bool wait_for_mutation, GuestDataLeaseDenialMetadata* denial)
{
	(void)wait_for_mutation;
	if (denial != nullptr) *denial = {};
	GuestDataLeaseDenialMetadata observation {};
	observation.address = address;
	observation.bytes = bytes;
	observation.required = mode;
	observation.acquisition_thread = reinterpret_cast<uintptr_t>(&g_android_guest_data);
	observation.generation = GetMutationGeneration();
	const auto deny = [&](GuestDataLeaseDenialReason reason) {
		observation.reason = reason;
		if (denial != nullptr) *denial = observation;
		return false;
	};
	const uint64_t host_address = address & ANDROID_DATA_ADDRESS_MASK;
	if (lease == nullptr || lease->IsActive() || address == 0 || host_address == 0 || bytes == 0 ||
	    address > UINT64_MAX - bytes || bytes > (std::numeric_limits<size_t>::max)() ||
	    bytes > ANDROID_DATA_ADDRESS_MASK - host_address ||
	    (mode != Mode::Read && mode != Mode::Write && mode != Mode::ReadWrite))
		return deny(GuestDataLeaseDenialReason::LeaseOrRangeInvalid);
	if (NativeClaimCoverageRequired() && !CurrentNativeClaimCoversGuestRange(address, bytes))
		return deny(GuestDataLeaseDenialReason::DataHandoffDenied);

	// Establish mapping lifetime authority before any logical or kernel query.
	AndroidGuestDataLeaseKind kind = AndroidGuestDataLeaseKind::None;
	if (!BeginAndroidGuestDataAdmission(&kind, &observation.retryable))
		return deny(observation.retryable ? GuestDataLeaseDenialReason::GateWriterPending
		                                 : GuestDataLeaseDenialReason::MappingLeaseCapabilityUnavailable);
	const uint64_t end = host_address + bytes;
	const bool logical = AndroidGuestDataLogicalAccess(host_address, end, mode);
	const bool bounded_kernel_read = mode == Mode::Read && bytes <= 256 &&
	    address == host_address && AndroidGuestDataKernelCopyEnabled();
	const bool physical = logical && (bounded_kernel_read
	    ? AndroidGuestDataKernelReadPhysicalAccess(host_address, bytes, &observation)
	    : AndroidGuestDataPhysicalAccess(host_address, end, mode, &observation));
	if (!physical)
	{
		EndAndroidGuestDataAdmission(kind);
		return deny(logical ? GuestDataLeaseDenialReason::PhysicalQueryOrRightsDenied
		                   : GuestDataLeaseDenialReason::LogicalRegistryDenied);
	}
	if (!GuestCpu::PrepareCpuMemoryAccess(host_address, bytes,
	    static_cast<GuestCpu::MemoryAccessIntent>(static_cast<uint32_t>(mode))))
	{
		EndAndroidGuestDataAdmission(kind);
		return deny(GuestDataLeaseDenialReason::DataHandoffDenied);
	}
	// Query kernel metadata without the top-byte tag, but preserve Bionic's
	// pointer tag for the eventual host access (including MTE-capable hosts).
	lease->host_begin = address;
	lease->byte_count = bytes;
	lease->mutation_generation = GetMutationGeneration();
	lease->acquiring_thread = observation.acquisition_thread;
	lease->android_admission = static_cast<uint8_t>(kind);
	return true;
}

void ReleaseGuestDataLease(GuestDataLease* lease)
{
	if (lease == nullptr || !lease->IsActive()) return;
	if (lease->acquiring_thread != reinterpret_cast<uintptr_t>(&g_android_guest_data) ||
	    g_android_guest_data.leases == 0)
		std::abort();
	const auto kind = static_cast<AndroidGuestDataLeaseKind>(lease->android_admission);
	lease->host_begin = 0;
	lease->byte_count = 0;
	lease->mutation_generation = 0;
	lease->acquiring_thread = 0;
	lease->android_admission = 0;
	EndAndroidGuestDataAdmission(kind);
}

bool CurrentThreadHasMemoryLease() noexcept
{
	return g_android_guest_data.leases != 0;
}

bool CaptureGuestDataRangeSeal(const GuestDataLease& lease, GuestDataRangeSeal* seal)
{
	if (seal == nullptr || seal->IsActive() || !lease.IsActive() ||
	    lease.acquiring_thread != reinterpret_cast<uintptr_t>(&g_android_guest_data) ||
	    g_android_guest_data.leases == 0) return false;
	const uint64_t begin = lease.HostBegin() & ANDROID_DATA_ADDRESS_MASK;
	if (begin == 0 || lease.ByteCount() > ANDROID_DATA_ADDRESS_MASK - begin) return false;
	const uint64_t end = begin + lease.ByteCount();
	// Native VMAs do not carry Core lifetime authority. Every byte captured by
	// the seal must be managed, even though a short lease can borrow native data.
	for (uint64_t cursor = begin; cursor < end;)
	{
		ManagedRegion region {};
		if (QueryManagedRegion(cursor, &region) != ManagedRegionState::Visible ||
		    region.begin > cursor || region.end <= cursor) return false;
		cursor = region.end < end ? region.end : end;
	}
	auto* registry = AndroidRangeSeals();
	if (registry == nullptr) return false;
	std::lock_guard<std::mutex> lock(registry->mutex);
	if (registry->next_ticket == 0) return false;
	if (registry->free_records == nullptr)
	{
		auto* block = new (std::nothrow) AndroidRangeSealBlock;
		if (block == nullptr) return false;
		block->next = registry->first.next;
		registry->first.next = block;
		registry->AddFreeRecords(block);
	}
	auto* record = registry->free_records;
	registry->free_records = record->next_free;
	*record = {registry->next_ticket++, begin, end, true, nullptr};
	seal->ticket = record->ticket;
	seal->host_begin = lease.HostBegin();
	seal->byte_count = lease.ByteCount();
	seal->original_generation = lease.MutationGeneration();
	seal->android_record = record;
	return true;
}

bool ValidateGuestDataRangeSeal(const GuestDataLease& lease, const GuestDataRangeSeal& seal)
{
	if (!seal.IsActive() || !lease.IsActive() || lease.HostBegin() != seal.host_begin ||
	    lease.ByteCount() != seal.byte_count ||
	    lease.acquiring_thread != reinterpret_cast<uintptr_t>(&g_android_guest_data) ||
	    g_android_guest_data.leases == 0) return false;
	auto* registry = AndroidRangeSeals();
	if (registry == nullptr) return false;
	std::lock_guard<std::mutex> lock(registry->mutex);
	const auto* record = static_cast<const AndroidRangeSealRecord*>(seal.android_record);
	return record != nullptr && record->ticket == seal.ticket && record->valid &&
	       record->begin == (seal.host_begin & ANDROID_DATA_ADDRESS_MASK) &&
	       record->end - record->begin == seal.byte_count;
}

void ReleaseGuestDataRangeSeal(GuestDataRangeSeal* seal)
{
	if (seal == nullptr || !seal->IsActive()) return;
	if (auto* registry = AndroidRangeSeals(); registry != nullptr)
	{
		std::lock_guard<std::mutex> lock(registry->mutex);
		auto* record = static_cast<AndroidRangeSealRecord*>(seal->android_record);
		if (record != nullptr && record->ticket == seal->ticket)
		{
			*record = {};
			record->next_free = registry->free_records;
			registry->free_records = record;
		}
	}
	seal->android_record = nullptr;
	seal->ticket = 0;
	seal->host_begin = 0;
	seal->byte_count = 0;
	seal->original_generation = 0;
}
#else
bool TryAcquireGuestDataLease(uint64_t address, uint64_t bytes, Mode mode, GuestDataLease*, bool,
                              GuestDataLeaseDenialMetadata* denial)
{
	if (denial != nullptr)
	{
		*denial = {};
		denial->address = address; denial->bytes = bytes; denial->required = mode;
		denial->reason = GuestDataLeaseDenialReason::MappingLeaseCapabilityUnavailable;
	}
	// Only the Apple registry currently pins guest mapping lifetime.
	return false;
}

void ReleaseGuestDataLease(GuestDataLease*) {}
bool CaptureGuestDataRangeSeal(const GuestDataLease&, GuestDataRangeSeal*) { return false; }
bool ValidateGuestDataRangeSeal(const GuestDataLease&, const GuestDataRangeSeal&) { return false; }
void ReleaseGuestDataRangeSeal(GuestDataRangeSeal* seal)
{
	if (seal == nullptr) return;
	seal->ticket = seal->host_begin = seal->byte_count = seal->original_generation = 0;
}
#endif

bool ValidateGuestDataLeaseRangeContinuity(const GuestDataLease&, uint64_t, uint64_t, uint64_t)
{
	// These backends do not expose an authoritative managed mapping reader gate.
	return false;
}

bool CheckGuestDataAccessUnderFexExecutionBarrier(uint64_t address, uint64_t size, bool write) noexcept
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (address == 0 || size == 0 || address > UINT64_MAX - size) return false;
	const auto end = address + size;
	const auto required = static_cast<uint32_t>(Mode::Read) |
	    (write ? static_cast<uint32_t>(Mode::Write) : 0U);
	while (address < end) {
		ManagedRegion region {};
		if (QueryAndroidGuestDataRegion(address, end, &region) != ManagedRegionState::Visible ||
		    region.begin > address || region.end <= address ||
		    (AndroidGuestDataRights(region.mode) & required) != required)
			return false;
		address = region.end < end ? region.end : end;
	}
	return true;
#else
	(void)address; (void)size; (void)write;
	return false;
#endif
}

bool CheckOwnedStackAccessUnderFexExecutionBarrier(uint64_t address, uint64_t size, bool write,
	uint64_t owned_begin, uint64_t owned_size) noexcept
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (owned_begin == 0 || owned_size == 0 || address < owned_begin || size == 0 ||
	    owned_size > UINT64_MAX - owned_begin || address - owned_begin >= owned_size ||
	    size > owned_size - (address - owned_begin)) return false;
	const auto end = address + size;
	const auto required = static_cast<uint32_t>(Mode::Read) |
	    (write ? static_cast<uint32_t>(Mode::Write) : 0U);
	while (address < end) {
		ManagedRegion region {};
		const auto state = QueryAndroidGuestDataRegion(address, end, &region);
		if (state == ManagedRegionState::Unmanaged) {
			// The synchronous caller supplies the allocator's exact live RW
			// interval. Check every page for managed guards/reservations.
			const auto last_page_byte = address | uint64_t {4095};
			address = last_page_byte >= end - 1 ? end : last_page_byte + 1;
			continue;
		}
		if (state != ManagedRegionState::Visible || region.begin > address || region.end <= address ||
		    (AndroidGuestDataRights(region.mode) & required) != required) return false;
		address = region.end < end ? region.end : end;
	}
	return true;
#else
	(void)address; (void)size; (void)write; (void)owned_begin; (void)owned_size;
	return false;
#endif
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
namespace {

constexpr uint64_t ANDROID_GUEST_PAGE_SIZE         = 4096;
constexpr uint64_t ANDROID_GUEST_SYSTEM_MIN        = UINT64_C(0x0000040000);
constexpr uint64_t ANDROID_GUEST_SYSTEM_MAX        = UINT64_C(0x07ffffbfff);
constexpr uint64_t ANDROID_GUEST_ANYWHERE_MIN      = UINT64_C(0x0000400000);
constexpr uint64_t ANDROID_GUEST_USER_MIN          = UINT64_C(0x1000000000);
constexpr uint64_t ANDROID_GUEST_USER_MAX          = UINT64_C(0xfbffffffff);

struct AndroidReservation
{
	uint64_t base     = 0;
	uint64_t size     = 0;
	uint64_t identity = 0;
};

struct AndroidReservedMapping
{
	uint64_t base                 = 0;
	uint64_t size                 = 0;
	uint64_t identity             = 0;
	uint64_t reservation_identity = 0;
	Mode*    page_modes           = nullptr;
	uint64_t page_mode_num        = 0;
	int      shared_backing_fd    = -1;
	uint64_t shared_backing_offset = 0;
};

struct AndroidReservationRegistry
{
	std::mutex              mutex {};
	AndroidReservation*     reservations              = nullptr;
	size_t                  reservation_num           = 0;
	size_t                  reservation_capacity      = 0;
	AndroidReservedMapping* mappings                  = nullptr;
	size_t                  mapping_num               = 0;
	size_t                  mapping_capacity          = 0;
	uint64_t                next_reservation_identity = 1;
	uint64_t                next_mapping_identity     = 1;

	~AndroidReservationRegistry()
	{
		for (size_t index = 0; index < mapping_num; index++)
		{
			std::free(mappings[index].page_modes);
		}
		std::free(mappings);
		std::free(reservations);
	}
};

AndroidReservationRegistry& AndroidRegistry()
{
	static AndroidReservationRegistry registry;
	return registry;
}

template <typename T>
bool EnsureAndroidRegistryCapacity(T** records, size_t count, size_t* capacity)
{
	if (records == nullptr || capacity == nullptr) return false;
	if (count < *capacity) return true;
	constexpr size_t initial_capacity = 8;
	const size_t max_capacity =
	    (std::numeric_limits<size_t>::max)() / sizeof(T);
	if (*capacity >= max_capacity) return false;
	const size_t new_capacity =
	    *capacity == 0
	        ? initial_capacity
	        : (*capacity > max_capacity / 2 ? max_capacity : *capacity * 2);
	if (new_capacity <= *capacity) return false;
	void* resized = std::realloc(*records, new_capacity * sizeof(T));
	if (resized == nullptr) return false;
	*records  = static_cast<T*>(resized);
	*capacity = new_capacity;
	return true;
}

bool AndroidRangeEnd(uint64_t base, uint64_t size, uint64_t* end)
{
	if (end == nullptr || size == 0 ||
	    base > (std::numeric_limits<uint64_t>::max)() - size)
	{
		return false;
	}
	*end = base + size;
	return true;
}

bool AndroidRangesOverlap(uint64_t first_base, uint64_t first_size,
	                      uint64_t second_base, uint64_t second_size)
{
	uint64_t first_end  = 0;
	uint64_t second_end = 0;
	return AndroidRangeEnd(first_base, first_size, &first_end) &&
	       AndroidRangeEnd(second_base, second_size, &second_end) &&
	       first_base < second_end && second_base < first_end;
}

uint64_t AndroidHostPageSize()
{
	static const uint64_t page_size = []
	{
		const long detected = sysconf(_SC_PAGESIZE);
		return detected == 4096 || detected == 16384
		           ? static_cast<uint64_t>(detected)
		           : uint64_t {0};
	}();
	return page_size;
}

bool AndroidValidMode(Mode mode)
{
	return static_cast<uint32_t>(mode) <=
	       static_cast<uint32_t>(Mode::ExecuteReadWrite);
}

int AndroidHostProtection(Mode mode)
{
	// Guest x86 code is translated by FEX and is never entered by the native
	// AArch64 CPU. In particular, even guest ExecuteWrite never creates a host
	// W+X mapping.
	switch (mode)
	{
		case Mode::Read:
		case Mode::Execute:
		case Mode::ExecuteRead: return PROT_READ;
		case Mode::Write:
		case Mode::ReadWrite:
		case Mode::ExecuteWrite:
		case Mode::ExecuteReadWrite: return PROT_READ | PROT_WRITE;
		case Mode::NoAccess:
		default: return PROT_NONE;
	}
}

bool AndroidIsPowerOfTwo(uint64_t value)
{
	return value != 0 && (value & (value - 1)) == 0;
}

bool AndroidAlignUp(uint64_t value, uint64_t alignment, uint64_t* result)
{
	if (result == nullptr || !AndroidIsPowerOfTwo(alignment) ||
	    value > (std::numeric_limits<uint64_t>::max)() - (alignment - 1))
	{
		return false;
	}
	*result = (value + alignment - 1) & ~(alignment - 1);
	return true;
}

bool AndroidGuestRangeContains(uint64_t begin, uint64_t size,
	                           uint64_t minimum, uint64_t maximum)
{
	return size != 0 && begin >= minimum && begin <= maximum &&
	       size - 1 <= maximum - begin;
}

bool AndroidFindGuestGap(uint64_t minimum, uint64_t maximum, uint64_t size,
	                     uint64_t alignment, uint64_t* result)
{
	uint64_t candidate = 0;
	if (result == nullptr || !AndroidAlignUp(minimum, alignment, &candidate))
	{
		return false;
	}

	FILE* const maps = std::fopen("/proc/self/maps", "r");
	if (maps == nullptr) return false;
	char line[1024] {};
	while (std::fgets(line, sizeof(line), maps) != nullptr)
	{
		uint64_t mapping_begin = 0;
		uint64_t mapping_end   = 0;
		if (std::sscanf(line, "%" SCNx64 "-%" SCNx64,
		                &mapping_begin, &mapping_end) != 2 ||
		    mapping_end <= candidate)
		{
			continue;
		}
		if (mapping_begin > maximum) break;
		if (mapping_begin > candidate && size <= mapping_begin - candidate)
		{
			break;
		}
		if (mapping_end > candidate &&
		    !AndroidAlignUp(mapping_end, alignment, &candidate))
		{
			candidate = 0;
			break;
		}
	}
	const bool found = AndroidGuestRangeContains(
	    candidate, size, minimum, maximum);
	std::fclose(maps);
	if (found) *result = candidate;
	return found;
}

void* AndroidMapExact(uint64_t address, uint64_t size, int protection,
	                  int extra_flags)
{
	if (address == 0 ||
	    address > (std::numeric_limits<uintptr_t>::max)() ||
	    size > (std::numeric_limits<size_t>::max)())
	{
		return MAP_FAILED;
	}
	void* mapping = mmap(
	    reinterpret_cast<void*>(static_cast<uintptr_t>(address)),
	    static_cast<size_t>(size), protection,
	    MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE | extra_flags, -1, 0);
	if (mapping == MAP_FAILED && errno == EINVAL)
	{
		// Older kernels can reject MAP_FIXED_NOREPLACE. A normal mmap hint never
		// replaces a live VMA; accept the fallback only when it returned the exact
		// address requested.
		mapping = mmap(
		    reinterpret_cast<void*>(static_cast<uintptr_t>(address)),
		    static_cast<size_t>(size), protection,
		    MAP_PRIVATE | MAP_ANON | extra_flags, -1, 0);
	}
	if (mapping == MAP_FAILED) return MAP_FAILED;
	if (reinterpret_cast<uint64_t>(mapping) == address) return mapping;
	(void)munmap(mapping, static_cast<size_t>(size));
	errno = EEXIST;
	return MAP_FAILED;
}

uint64_t AndroidReserveAnywhere(uint64_t size, uint64_t alignment)
{
	struct GuestWindow
	{
		uint64_t minimum;
		uint64_t maximum;
	};
	constexpr GuestWindow windows[] = {
	    {ANDROID_GUEST_ANYWHERE_MIN, ANDROID_GUEST_SYSTEM_MAX},
	    {ANDROID_GUEST_USER_MIN, ANDROID_GUEST_USER_MAX}};
	for (const auto& window: windows)
	{
		uint64_t minimum = window.minimum;
		for (;;)
		{
			uint64_t candidate = 0;
			if (!AndroidFindGuestGap(minimum, window.maximum, size,
			                         alignment, &candidate))
			{
				break;
			}
			void* mapping = AndroidMapExact(
			    candidate, size, PROT_NONE, MAP_NORESERVE);
			if (mapping != MAP_FAILED) return candidate;
			if (errno != EEXIST) break;
			if (candidate > window.maximum - alignment) break;
			minimum = candidate + alignment;
		}
	}
	return 0;
}

uint64_t AndroidReserveHost(uint64_t address, uint64_t size,
	                        uint64_t alignment,
	                        ReservationPlacement placement)
{
	if (address != 0 &&
	    (placement == ReservationPlacement::Exact ||
	     address >= ANDROID_GUEST_ANYWHERE_MIN))
	{
		if (AndroidMapExact(address, size, PROT_NONE, MAP_NORESERVE) !=
		    MAP_FAILED)
		{
			return address;
		}
		if (placement == ReservationPlacement::Exact) return 0;
	}
	return AndroidReserveAnywhere(size, alignment);
}

AndroidReservation* FindAndroidContainingReservation(
	AndroidReservationRegistry& registry, uint64_t address, uint64_t size)
{
	uint64_t requested_end = 0;
	if (!AndroidRangeEnd(address, size, &requested_end)) return nullptr;
	for (size_t index = 0; index < registry.reservation_num; index++)
	{
		auto& reservation = registry.reservations[index];
		uint64_t reservation_end = 0;
		if (AndroidRangeEnd(reservation.base, reservation.size,
		                    &reservation_end) &&
		    address >= reservation.base && requested_end <= reservation_end)
		{
			return &reservation;
		}
	}
	return nullptr;
}

AndroidReservedMapping* FindAndroidContainingMapping(
	AndroidReservationRegistry& registry, uint64_t address, uint64_t size)
{
	uint64_t requested_end = 0;
	if (!AndroidRangeEnd(address, size, &requested_end)) return nullptr;
	for (size_t index = 0; index < registry.mapping_num; index++)
	{
		auto& mapping = registry.mappings[index];
		uint64_t mapping_end = 0;
		if (AndroidRangeEnd(mapping.base, mapping.size, &mapping_end) &&
		    address >= mapping.base && requested_end <= mapping_end)
		{
			return &mapping;
		}
	}
	return nullptr;
}

bool AnyAndroidReservationOverlap(AndroidReservationRegistry& registry,
	                              uint64_t address, uint64_t size)
{
	for (size_t index = 0; index < registry.reservation_num; index++)
	{
		if (AndroidRangesOverlap(address, size,
		                         registry.reservations[index].base,
		                         registry.reservations[index].size))
		{
			return true;
		}
	}
	return false;
}

bool AnyAndroidMappingOverlap(AndroidReservationRegistry& registry,
	                          uint64_t address, uint64_t size)
{
	for (size_t index = 0; index < registry.mapping_num; index++)
	{
		if (AndroidRangesOverlap(address, size, registry.mappings[index].base,
		                         registry.mappings[index].size))
		{
			return true;
		}
	}
	return false;
}

size_t FindAndroidReservationHandle(
	AndroidReservationRegistry& registry, const ReservationHandle& handle)
{
	for (size_t index = 0; index < registry.reservation_num; index++)
	{
		const auto& reservation = registry.reservations[index];
		if (reservation.base == handle.base &&
		    reservation.size == handle.size &&
		    reservation.identity == handle.identity)
		{
			return index;
		}
	}
	return registry.reservation_num;
}

size_t FindAndroidMappingIdentity(AndroidReservationRegistry& registry,
	                              uint64_t identity)
{
	for (size_t index = 0; index < registry.mapping_num; index++)
	{
		if (registry.mappings[index].identity == identity) return index;
	}
	return registry.mapping_num;
}

void EraseAndroidReservation(AndroidReservationRegistry& registry,
	                         size_t index)
{
	if (index >= registry.reservation_num) return;
	if (index + 1 < registry.reservation_num)
	{
		std::memmove(&registry.reservations[index],
		             &registry.reservations[index + 1],
		             (registry.reservation_num - index - 1) *
		                 sizeof(AndroidReservation));
	}
	registry.reservation_num--;
}

void EraseAndroidMapping(AndroidReservationRegistry& registry, size_t index)
{
	if (index >= registry.mapping_num) return;
	std::free(registry.mappings[index].page_modes);
	if (index + 1 < registry.mapping_num)
	{
		std::memmove(&registry.mappings[index], &registry.mappings[index + 1],
		             (registry.mapping_num - index - 1) *
		                 sizeof(AndroidReservedMapping));
	}
	registry.mapping_num--;
}

bool AndroidRestoreMappingProtection(const AndroidReservedMapping& mapping)
{
	const uint64_t host_page_size = AndroidHostPageSize();
	if (host_page_size == 0 || mapping.base % host_page_size != 0 ||
	    mapping.size % host_page_size != 0)
	{
		return false;
	}
	for (uint64_t page = 0; page < mapping.size; page += host_page_size)
	{
		const uint64_t page_index = page / ANDROID_GUEST_PAGE_SIZE;
		if (page_index >= mapping.page_mode_num ||
		    mprotect(
		        reinterpret_cast<void*>(static_cast<uintptr_t>(mapping.base + page)),
		        static_cast<size_t>(host_page_size),
		        AndroidHostProtection(
		            mapping.page_modes[static_cast<size_t>(page_index)])) != 0)
		{
			return false;
		}
	}
	return true;
}

bool AndroidActivateReservationRange(uint64_t address, uint64_t size,
	                                  Mode mode)
{
	InvalidateAndroidRangeSeals(address, size);
	if (mprotect(reinterpret_cast<void*>(static_cast<uintptr_t>(address)),
	             static_cast<size_t>(size), AndroidHostProtection(mode)) == 0)
	{
		return true;
	}
	// mprotect can change an earlier VMA before failing. The complete range was
	// a PROT_NONE reservation, so restore that invariant before returning.
	if (mprotect(reinterpret_cast<void*>(static_cast<uintptr_t>(address)),
	             static_cast<size_t>(size), PROT_NONE) != 0)
	{
		std::abort();
	}
	return false;
}

bool AndroidDecommitReservedMapping(const AndroidReservedMapping& mapping)
{
	InvalidateAndroidRangeSeals(mapping.base, mapping.size);
	void* const address =
	    reinterpret_cast<void*>(static_cast<uintptr_t>(mapping.base));
	if (mapping.shared_backing_fd >= 0)
	{
		// A PROT_NONE shared VMA would still hold the physical file alive after
		// its allocation is released. Restore the reservation's anonymous hole
		// while this complete child extent remains exclusively owned.
		if (mmap(address, static_cast<size_t>(mapping.size), PROT_NONE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == address) return true;
		// A refused replacement must leave the original data and every logical
		// protection truthful. Restore with no access, then the existing modes.
		if (mmap(address, static_cast<size_t>(mapping.size), PROT_NONE,
		    MAP_SHARED | MAP_FIXED, mapping.shared_backing_fd,
		    static_cast<off_t>(mapping.shared_backing_offset)) != address ||
		    !AndroidRestoreMappingProtection(mapping)) std::abort();
		return false;
	}
	if (mprotect(address, static_cast<size_t>(mapping.size), PROT_NONE) != 0)
	{
		// Registry modes still describe the live mapping. Restore every complete
		// host page before releasing the registry/FEX exclusion.
		if (!AndroidRestoreMappingProtection(mapping)) std::abort();
		return false;
	}
	if (madvise(address, static_cast<size_t>(mapping.size), MADV_DONTNEED) != 0)
	{
		// A valid registered range should not fail DONTNEED. Restore access for a
		// recoverable kernel refusal; abort if the registry can no longer be made
		// truthful.
		if (!AndroidRestoreMappingProtection(mapping)) std::abort();
		return false;
	}
	return true;
}

enum class AndroidTransactionKind : uint32_t
{
	Reservation,
	ReservedMapping,
};

thread_local AndroidReservationRegistry*
	g_android_registry_transaction_owner = nullptr;

class AndroidRegistryTransactionOwner final
{
public:
	bool Acquire(AndroidReservationRegistry* selected_registry)
	{
		if (selected_registry == nullptr || registry != nullptr ||
		    g_android_registry_transaction_owner != nullptr)
		{
			return false;
		}
		registry = selected_registry;
		g_android_registry_transaction_owner = selected_registry;
		return true;
	}

	~AndroidRegistryTransactionOwner()
	{
		if (registry == nullptr) return;
		if (g_android_registry_transaction_owner != registry) std::abort();
		g_android_registry_transaction_owner = nullptr;
	}

	AndroidRegistryTransactionOwner() = default;
	AndroidRegistryTransactionOwner(const AndroidRegistryTransactionOwner&) =
		delete;
	AndroidRegistryTransactionOwner& operator=(
		const AndroidRegistryTransactionOwner&) = delete;

private:
	AndroidReservationRegistry* registry = nullptr;
};

std::unique_lock<std::mutex> LockAndroidRegistryForQuery(
	AndroidReservationRegistry& registry)
{
	std::unique_lock<std::mutex> lock(registry.mutex, std::defer_lock);
	// The transaction owner already holds this non-recursive mutex. This
	// read-only path is needed when final guest-output validation misses its TLS
	// translation cache after BeginReserveAligned recorded the mutation.
	if (g_android_registry_transaction_owner != &registry) lock.lock();
	return lock;
}

struct AndroidMutationTransactionState
{
	explicit AndroidMutationTransactionState(
	    AndroidReservationRegistry& selected_registry,
	    FexGuestMemoryMutationKind mutation_kind)
	    : registry(&selected_registry),
	      lock(selected_registry.mutex, std::defer_lock),
	      fex(mutation_kind)
	{
		// The external FEX barrier is always entered before the local registry
		// gate. Reverse member destruction closes FEX, clears the thread-owner
		// marker, and only then releases the mutex.
		if (fex.IsAdmitted() &&
		    g_android_registry_transaction_owner == nullptr)
		{
			lock.lock();
			if (!owner.Acquire(&selected_registry)) lock.unlock();
		}
	}

	bool IsAdmitted() const { return fex.IsAdmitted() && lock.owns_lock(); }

	AndroidReservationRegistry* registry = nullptr;
	std::unique_lock<std::mutex> lock;
	AndroidRegistryTransactionOwner owner;
	FexGuestMemoryMutation       fex;
	AndroidTransactionKind       kind = AndroidTransactionKind::Reservation;
	ReservationHandle            reservation {};
	uint64_t                     mapping_identity = 0;
	uint64_t                     range_base = 0;
	uint64_t                     range_size = 0;
};

bool AbortAndroidTransaction(AndroidMutationTransactionState& transaction)
{
	auto& registry = *transaction.registry;
	if (transaction.kind == AndroidTransactionKind::Reservation)
	{
		const size_t reservation_index =
		    FindAndroidReservationHandle(registry, transaction.reservation);
		if (reservation_index >= registry.reservation_num) return false;
		InvalidateAndroidRangeSeals(transaction.reservation.base, transaction.reservation.size);
		if (munmap(reinterpret_cast<void*>(static_cast<uintptr_t>(
		               transaction.reservation.base)),
		           static_cast<size_t>(transaction.reservation.size)) != 0)
		{
			transaction.fex.Commit(transaction.range_base,
			                       transaction.range_size);
			return false;
		}
		for (size_t index = registry.mapping_num; index != 0; index--)
		{
			if (registry.mappings[index - 1].reservation_identity ==
			    transaction.reservation.identity)
			{
				EraseAndroidMapping(registry, index - 1);
			}
		}
		EraseAndroidReservation(registry, reservation_index);
		RecordMutation();
		return true;
	}

	const size_t mapping_index =
	    FindAndroidMappingIdentity(registry, transaction.mapping_identity);
	if (mapping_index >= registry.mapping_num) return false;
	if (!AndroidDecommitReservedMapping(registry.mappings[mapping_index]))
	{
		transaction.fex.Commit(transaction.range_base,
		                       transaction.range_size);
		return false;
	}
	EraseAndroidMapping(registry, mapping_index);
	RecordMutation();
	return true;
}

enum class AndroidReservedFreeResult : uint32_t
{
	NotReserved,
	Failed,
	Released,
};

AndroidReservedFreeResult FreeAndroidReservedMapping(
	uint64_t address, uint64_t* released_size)
{
	if (released_size != nullptr) *released_size = 0;
	auto& registry = AndroidRegistry();
	std::lock_guard<std::mutex> lock(registry.mutex);
	for (size_t index = 0; index < registry.mapping_num; index++)
	{
		const auto mapping = registry.mappings[index];
		if (mapping.base != address) continue;
		if (!AndroidDecommitReservedMapping(mapping))
		{
			return AndroidReservedFreeResult::Failed;
		}
		EraseAndroidMapping(registry, index);
		RecordMutation();
		if (released_size != nullptr) *released_size = mapping.size;
		return AndroidReservedFreeResult::Released;
	}
	return AnyAndroidReservationOverlap(registry, address, 1)
	           ? AndroidReservedFreeResult::Failed
	           : AndroidReservedFreeResult::NotReserved;
}

enum class AndroidReservedProtectResult : uint32_t
{
	NotReserved,
	Failed,
	Protected,
};

AndroidReservedProtectResult ProtectAndroidReservedMapping(
	uint64_t address, uint64_t size, Mode mode, Mode* old_mode)
{
	auto& registry = AndroidRegistry();
	std::lock_guard<std::mutex> lock(registry.mutex);
	if (!AnyAndroidReservationOverlap(registry, address, size))
	{
		return AndroidReservedProtectResult::NotReserved;
	}
	uint64_t requested_end = 0;
	const uint64_t host_page_size = AndroidHostPageSize();
	if (!AndroidValidMode(mode) || host_page_size == 0 ||
	    !AndroidRangeEnd(address, size, &requested_end) ||
	    requested_end > (std::numeric_limits<uint64_t>::max)() -
	                        (ANDROID_GUEST_PAGE_SIZE - 1))
	{
		return AndroidReservedProtectResult::Failed;
	}
	const uint64_t guest_begin =
	    address & ~(ANDROID_GUEST_PAGE_SIZE - 1);
	const uint64_t guest_end =
	    (requested_end + ANDROID_GUEST_PAGE_SIZE - 1) &
	    ~(ANDROID_GUEST_PAGE_SIZE - 1);
	AndroidReservedMapping* mapping =
	    FindAndroidContainingMapping(registry, guest_begin,
	                                 guest_end - guest_begin);
	if (mapping == nullptr) return AndroidReservedProtectResult::Failed;

	const uint64_t physical_begin = address & ~(host_page_size - 1);
	if (requested_end > (std::numeric_limits<uint64_t>::max)() -
	                        (host_page_size - 1))
	{
		return AndroidReservedProtectResult::Failed;
	}
	const uint64_t physical_end =
	    (requested_end + host_page_size - 1) & ~(host_page_size - 1);
	const uint64_t first_page =
	    (guest_begin - mapping->base) / ANDROID_GUEST_PAGE_SIZE;
	const uint64_t page_num =
	    (guest_end - guest_begin) / ANDROID_GUEST_PAGE_SIZE;
	if (first_page >= mapping->page_mode_num ||
	    page_num > mapping->page_mode_num - first_page)
	{
		return AndroidReservedProtectResult::Failed;
	}

	const int desired_host_protection = AndroidHostProtection(mode);
	for (uint64_t page_address = physical_begin;
	     page_address < physical_end;
	     page_address += ANDROID_GUEST_PAGE_SIZE)
	{
		if (page_address >= guest_begin && page_address < guest_end) continue;
		const uint64_t page_index =
		    (page_address - mapping->base) / ANDROID_GUEST_PAGE_SIZE;
		if (page_index >= mapping->page_mode_num ||
		    AndroidHostProtection(
		        mapping->page_modes[static_cast<size_t>(page_index)]) !=
		        desired_host_protection)
		{
			return AndroidReservedProtectResult::Failed;
		}
	}

	const Mode previous = mapping->page_modes[static_cast<size_t>(first_page)];
	InvalidateAndroidRangeSeals(physical_begin, physical_end - physical_begin);
	if (mprotect(reinterpret_cast<void*>(static_cast<uintptr_t>(physical_begin)),
	             static_cast<size_t>(physical_end - physical_begin),
	             desired_host_protection) != 0)
	{
		// mprotect may have changed an earlier VMA before failing on a later
		// one. Registry modes are still the originals, so restore each complete
		// host page before the FEX/registry gates are released.
		for (uint64_t host_page = physical_begin;
		     host_page < physical_end; host_page += host_page_size)
		{
			const uint64_t page_index =
			    (host_page - mapping->base) / ANDROID_GUEST_PAGE_SIZE;
			if (page_index >= mapping->page_mode_num ||
			    mprotect(
			        reinterpret_cast<void*>(static_cast<uintptr_t>(host_page)),
			        static_cast<size_t>(host_page_size),
			        AndroidHostProtection(mapping->page_modes[
			            static_cast<size_t>(page_index)])) != 0)
			{
				std::abort();
			}
		}
		return AndroidReservedProtectResult::Failed;
	}

	for (uint64_t page = 0; page < page_num; page++)
	{
		mapping->page_modes[static_cast<size_t>(first_page + page)] = mode;
	}
	if (old_mode != nullptr) *old_mode = previous;
	return AndroidReservedProtectResult::Protected;
}

ManagedRegionState QueryAndroidReservedRegion(uint64_t address,
	                                          ManagedRegion* region,
	                                          bool* handled,
	                                          uint64_t requested_end = 0)
{
	if (handled != nullptr) *handled = false;
	auto& registry = AndroidRegistry();
	auto lock = LockAndroidRegistryForQuery(registry);
	AndroidReservation* reservation =
	    FindAndroidContainingReservation(registry, address, 1);
	if (reservation == nullptr) return ManagedRegionState::Unmanaged;
	if (handled != nullptr) *handled = true;

	AndroidReservedMapping* mapping =
	    FindAndroidContainingMapping(registry, address, 1);
	if (mapping != nullptr)
	{
		const uint64_t target_page =
		    (address - mapping->base) / ANDROID_GUEST_PAGE_SIZE;
		const Mode mode =
		    mapping->page_modes[static_cast<size_t>(target_page)];
		uint64_t first_page = target_page;
		uint64_t last_page  = target_page;
		const uint64_t requested_last_page = requested_end == 0 ? mapping->page_mode_num - 1 :
		    std::min(mapping->page_mode_num - 1, (requested_end - 1 - mapping->base) / ANDROID_GUEST_PAGE_SIZE);
		while (requested_end == 0 && first_page != 0 &&
		       mapping->page_modes[static_cast<size_t>(first_page - 1)] == mode)
		{
			first_page--;
		}
		while (last_page < requested_last_page &&
		       mapping->page_modes[static_cast<size_t>(last_page + 1)] == mode)
		{
			last_page++;
		}
		if (region != nullptr)
		{
			*region = ManagedRegion {
			    mapping->base + first_page * ANDROID_GUEST_PAGE_SIZE,
			    mapping->base + (last_page + 1) * ANDROID_GUEST_PAGE_SIZE,
			    mode};
			if (requested_end != 0)
			{
				region->begin = address;
				if (region->end > requested_end) region->end = requested_end;
			}
		}
		return ManagedRegionState::Visible;
	}

	uint64_t hole_begin = reservation->base;
	uint64_t hole_end   = reservation->base + reservation->size;
	for (size_t index = 0; index < registry.mapping_num; index++)
	{
		const auto& sibling = registry.mappings[index];
		if (sibling.reservation_identity != reservation->identity) continue;
		const uint64_t sibling_end = sibling.base + sibling.size;
		if (sibling_end <= address && sibling_end > hole_begin)
		{
			hole_begin = sibling_end;
		}
		if (sibling.base > address && sibling.base < hole_end)
		{
			hole_end = sibling.base;
		}
	}
	if (region != nullptr)
	{
		*region = ManagedRegion {hole_begin, hole_end, Mode::NoAccess};
	}
	return ManagedRegionState::Reserved;
}

ManagedRegionState QueryAndroidGuestDataRegion(uint64_t address, uint64_t end, ManagedRegion* region)
{
	// Data consumers need only the requested bytes, not the full historical
	// equal-mode run returned by QueryManagedRegion. Keep the same registry
	// ownership and admission; this result grants no rights across calls.
	if (region != nullptr) *region = ManagedRegion {0, 0, Mode::NoAccess};
	if (address == 0 || end <= address) return ManagedRegionState::Unmanaged;
	bool handled = false;
	const auto reserved_state = QueryAndroidReservedRegion(address, region, &handled, end);
	if (handled) return reserved_state;
	return sys_virtual_query_managed_data_region(address, end, region);
}

bool IsAndroidReservedManagedAddress(uint64_t address, bool* handled)
{
	if (handled != nullptr) *handled = false;
	auto& registry = AndroidRegistry();
	auto lock = LockAndroidRegistryForQuery(registry);
	if (FindAndroidContainingReservation(registry, address, 1) == nullptr)
	{
		return false;
	}
	if (handled != nullptr) *handled = true;
	return FindAndroidContainingMapping(registry, address, 1) != nullptr;
}

} // namespace
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64
namespace {

struct WindowsReservation
{
	uint64_t base     = 0;
	uint64_t size     = 0;
	uint64_t identity = 0;
};

struct WindowsReservedMapping
{
	uint64_t base                 = 0;
	uint64_t size                 = 0;
	uint64_t reservation_identity = 0;
};

struct WindowsReservationRegistry
{
	std::mutex              mutex {};
	WindowsReservation*     reservations             = nullptr;
	size_t                  reservation_num          = 0;
	size_t                  reservation_capacity     = 0;
	WindowsReservedMapping* mappings                 = nullptr;
	size_t                  mapping_num              = 0;
	size_t                  mapping_capacity         = 0;
	uint64_t                next_reservation_identity = 1;

	~WindowsReservationRegistry()
	{
		std::free(mappings);
		std::free(reservations);
	}
};

WindowsReservationRegistry& WindowsRegistry()
{
	static WindowsReservationRegistry registry;
	return registry;
}

template <typename T>
bool EnsureWindowsRegistryCapacity(T** records, size_t count, size_t* capacity)
{
	if (records == nullptr || capacity == nullptr) return false;
	if (count < *capacity) return true;
	constexpr size_t initial_capacity = 8;
	const size_t max_capacity = (std::numeric_limits<size_t>::max)() / sizeof(T);
	if (*capacity >= max_capacity) return false;
	const size_t new_capacity =
	    *capacity == 0
	        ? initial_capacity
	        : (*capacity > max_capacity / 2 ? max_capacity : *capacity * 2);
	if (new_capacity <= *capacity) return false;
	void* resized = std::realloc(*records, new_capacity * sizeof(T));
	if (resized == nullptr) return false;
	*records  = static_cast<T*>(resized);
	*capacity = new_capacity;
	return true;
}

bool WindowsRangeEnd(uint64_t base, uint64_t size, uint64_t* end)
{
	if (end == nullptr || size == 0 || base > (std::numeric_limits<uint64_t>::max)() - size) return false;
	*end = base + size;
	return true;
}

bool WindowsRangesOverlap(uint64_t first_base, uint64_t first_size, uint64_t second_base, uint64_t second_size)
{
	uint64_t first_end  = 0;
	uint64_t second_end = 0;
	return WindowsRangeEnd(first_base, first_size, &first_end) &&
	       WindowsRangeEnd(second_base, second_size, &second_end) &&
	       first_base < second_end && second_base < first_end;
}

bool WindowsValidMode(Mode mode)
{
	return static_cast<uint32_t>(mode) <= static_cast<uint32_t>(Mode::ExecuteReadWrite);
}

DWORD WindowsProtection(Mode mode)
{
	switch (mode)
	{
		case Mode::Read: return PAGE_READONLY;
		case Mode::Write:
		case Mode::ReadWrite: return PAGE_READWRITE;
		case Mode::Execute: return PAGE_EXECUTE;
		case Mode::ExecuteRead: return PAGE_EXECUTE_READ;
		case Mode::ExecuteWrite:
		case Mode::ExecuteReadWrite: return PAGE_EXECUTE_READWRITE;
		case Mode::NoAccess:
		default: return PAGE_NOACCESS;
	}
}

uint64_t WindowsHostPageSize()
{
	static const uint64_t page_size = []
	{
		SYSTEM_INFO info {};
		::GetSystemInfo(&info);
		return info.dwPageSize != 0 ? static_cast<uint64_t>(info.dwPageSize) : uint64_t {4096};
	}();
	return page_size;
}

uint64_t WindowsAllocationGranularity()
{
	static const uint64_t granularity = []
	{
		SYSTEM_INFO info {};
		::GetSystemInfo(&info);
		return info.dwAllocationGranularity != 0
		           ? static_cast<uint64_t>(info.dwAllocationGranularity)
		           : uint64_t {65536};
	}();
	return granularity;
}

using WindowsVirtualAlloc2 = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);

WindowsVirtualAlloc2 ResolveWindowsVirtualAlloc2()
{
	HMODULE module = GetModuleHandleA("KernelBase.dll");
	return module != nullptr
	           ? reinterpret_cast<WindowsVirtualAlloc2>(GetProcAddress(module, "VirtualAlloc2"))
	           : nullptr;
}

uint64_t WindowsReserveAnywhere(uint64_t size, uint64_t alignment)
{
	static constexpr uint64_t user_min = 0x1000000000ull;
	static constexpr uint64_t user_max = 0xFBFFFFFFFFull;
	static const auto virtual_alloc2 = ResolveWindowsVirtualAlloc2();
	if (virtual_alloc2 == nullptr || size > (std::numeric_limits<SIZE_T>::max)()) return 0;

	const uint64_t required_alignment =
	    alignment < WindowsAllocationGranularity() ? WindowsAllocationGranularity() : alignment;
	MEM_ADDRESS_REQUIREMENTS requirements {};
	requirements.LowestStartingAddress = reinterpret_cast<PVOID>(static_cast<uintptr_t>(user_min));
	requirements.HighestEndingAddress  = reinterpret_cast<PVOID>(static_cast<uintptr_t>(user_max));
	requirements.Alignment             = static_cast<SIZE_T>(required_alignment);
	MEM_EXTENDED_PARAMETER parameter {};
	parameter.Type    = MemExtendedParameterAddressRequirements;
	parameter.Pointer = &requirements;

	auto* allocation = virtual_alloc2(GetCurrentProcess(), nullptr, static_cast<SIZE_T>(size), MEM_RESERVE,
	                                  PAGE_NOACCESS, &parameter, 1);
	if (allocation == nullptr) return 0;
	const uint64_t base = reinterpret_cast<uint64_t>(allocation);
	uint64_t       end  = 0;
	if (base % alignment != 0 || !WindowsRangeEnd(base, size, &end) || base < user_min || end - 1 > user_max)
	{
		(void)VirtualFree(allocation, 0, MEM_RELEASE);
		return 0;
	}
	return base;
}

uint64_t WindowsReserveHost(uint64_t address, uint64_t size, uint64_t alignment, ReservationPlacement placement)
{
	if (size > (std::numeric_limits<SIZE_T>::max)()) return 0;
	if (address != 0)
	{
		auto* allocation = VirtualAlloc(reinterpret_cast<void*>(static_cast<uintptr_t>(address)),
		                                static_cast<SIZE_T>(size), MEM_RESERVE, PAGE_NOACCESS);
		if (allocation != nullptr)
		{
			const uint64_t base = reinterpret_cast<uint64_t>(allocation);
			if (base == address) return base;
			(void)VirtualFree(allocation, 0, MEM_RELEASE);
		}
		if (placement == ReservationPlacement::Exact) return 0;
	}
	return WindowsReserveAnywhere(size, alignment);
}

WindowsReservation* FindWindowsContainingReservation(WindowsReservationRegistry& registry, uint64_t address, uint64_t size)
{
	uint64_t requested_end = 0;
	if (!WindowsRangeEnd(address, size, &requested_end)) return nullptr;
	for (size_t index = 0; index < registry.reservation_num; index++)
	{
		auto&    reservation = registry.reservations[index];
		uint64_t end         = 0;
		if (WindowsRangeEnd(reservation.base, reservation.size, &end) &&
		    address >= reservation.base && requested_end <= end)
		{
			return &reservation;
		}
	}
	return nullptr;
}

bool AnyWindowsReservationOverlap(WindowsReservationRegistry& registry, uint64_t address, uint64_t size)
{
	for (size_t index = 0; index < registry.reservation_num; index++)
	{
		if (WindowsRangesOverlap(address, size, registry.reservations[index].base, registry.reservations[index].size)) return true;
	}
	return false;
}

bool AnyWindowsMappingOverlap(WindowsReservationRegistry& registry, uint64_t address, uint64_t size)
{
	for (size_t index = 0; index < registry.mapping_num; index++)
	{
		if (WindowsRangesOverlap(address, size, registry.mappings[index].base, registry.mappings[index].size)) return true;
	}
	return false;
}

size_t FindWindowsReservationHandle(WindowsReservationRegistry& registry, const ReservationHandle& handle)
{
	for (size_t index = 0; index < registry.reservation_num; index++)
	{
		const auto& reservation = registry.reservations[index];
		if (reservation.base == handle.base && reservation.size == handle.size && reservation.identity == handle.identity)
			return index;
	}
	return registry.reservation_num;
}

void EraseWindowsReservation(WindowsReservationRegistry& registry, size_t index)
{
	if (index >= registry.reservation_num) return;
	if (index + 1 < registry.reservation_num)
	{
		std::memmove(&registry.reservations[index], &registry.reservations[index + 1],
		             (registry.reservation_num - index - 1) * sizeof(WindowsReservation));
	}
	registry.reservation_num--;
}

void EraseWindowsMapping(WindowsReservationRegistry& registry, size_t index)
{
	if (index >= registry.mapping_num) return;
	if (index + 1 < registry.mapping_num)
	{
		std::memmove(&registry.mappings[index], &registry.mappings[index + 1],
		             (registry.mapping_num - index - 1) * sizeof(WindowsReservedMapping));
	}
	registry.mapping_num--;
}

enum class WindowsTransactionKind : uint32_t
{
	Reservation,
	ReservedMapping,
};

struct WindowsMutationTransactionState
{
	explicit WindowsMutationTransactionState(WindowsReservationRegistry& registry)
	    : registry(&registry), lock(registry.mutex)
	{
	}

	WindowsReservationRegistry* registry = nullptr;
	std::unique_lock<std::mutex> lock;
	WindowsTransactionKind      kind = WindowsTransactionKind::Reservation;
	ReservationHandle           reservation {};
	WindowsReservedMapping      mapping {};
};

bool AbortWindowsTransaction(WindowsMutationTransactionState& transaction)
{
	auto& registry = *transaction.registry;
	if (transaction.kind == WindowsTransactionKind::Reservation)
	{
		const size_t reservation_index = FindWindowsReservationHandle(registry, transaction.reservation);
		if (reservation_index >= registry.reservation_num ||
		    VirtualFree(reinterpret_cast<void*>(static_cast<uintptr_t>(transaction.reservation.base)), 0, MEM_RELEASE) == 0)
		{
			return false;
		}
		for (size_t index = registry.mapping_num; index != 0; index--)
		{
			if (registry.mappings[index - 1].reservation_identity == transaction.reservation.identity)
				EraseWindowsMapping(registry, index - 1);
		}
		EraseWindowsReservation(registry, reservation_index);
		RecordMutation();
		return true;
	}

	for (size_t index = 0; index < registry.mapping_num; index++)
	{
		const auto& mapping = registry.mappings[index];
		if (mapping.base != transaction.mapping.base || mapping.size != transaction.mapping.size ||
		    mapping.reservation_identity != transaction.mapping.reservation_identity)
		{
			continue;
		}
		if (VirtualFree(reinterpret_cast<void*>(static_cast<uintptr_t>(mapping.base)),
		                static_cast<SIZE_T>(mapping.size), MEM_DECOMMIT) == 0)
		{
			return false;
		}
		EraseWindowsMapping(registry, index);
		RecordMutation();
		return true;
	}
	return false;
}

enum class WindowsReservedFreeResult : uint32_t
{
	NotReserved,
	Failed,
	Released,
};

WindowsReservedFreeResult FreeWindowsReservedMapping(uint64_t address, uint64_t* released_size)
{
	if (released_size != nullptr) *released_size = 0;
	auto& registry = WindowsRegistry();
	std::lock_guard<std::mutex> lock(registry.mutex);
	for (size_t index = 0; index < registry.mapping_num; index++)
	{
		const auto mapping = registry.mappings[index];
		if (mapping.base != address) continue;
		if (VirtualFree(reinterpret_cast<void*>(static_cast<uintptr_t>(mapping.base)),
		                static_cast<SIZE_T>(mapping.size), MEM_DECOMMIT) == 0)
		{
			return WindowsReservedFreeResult::Failed;
		}
		EraseWindowsMapping(registry, index);
		RecordMutation();
		if (released_size != nullptr) *released_size = mapping.size;
		return WindowsReservedFreeResult::Released;
	}
	return AnyWindowsReservationOverlap(registry, address, 1)
	           ? WindowsReservedFreeResult::Failed
	           : WindowsReservedFreeResult::NotReserved;
}

enum class WindowsReservedProtectResult : uint32_t
{
	NotReserved,
	Failed,
	Protected,
};

WindowsReservedProtectResult ProtectWindowsReservedMapping(uint64_t address, uint64_t size, Mode mode, Mode* old_mode)
{
	auto& registry = WindowsRegistry();
	std::lock_guard<std::mutex> lock(registry.mutex);
	if (!AnyWindowsReservationOverlap(registry, address, size))
	{
		return WindowsReservedProtectResult::NotReserved;
	}
	uint64_t requested_end = 0;
	if (!WindowsValidMode(mode) || !WindowsRangeEnd(address, size, &requested_end))
	{
		return WindowsReservedProtectResult::Failed;
	}
	for (size_t index = 0; index < registry.mapping_num; index++)
	{
		const auto& mapping = registry.mappings[index];
		uint64_t    mapping_end = 0;
		if (WindowsRangeEnd(mapping.base, mapping.size, &mapping_end) &&
		    address >= mapping.base && requested_end <= mapping_end)
		{
			return sys_virtual_protect(address, size, mode, old_mode)
			           ? WindowsReservedProtectResult::Protected
			           : WindowsReservedProtectResult::Failed;
		}
	}
	return WindowsReservedProtectResult::Failed;
}

} // namespace
#endif

MutationTransaction::~MutationTransaction()
{
	(void)Abort();
}

void MutationTransaction::Commit()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	auto* transaction = static_cast<AndroidMutationTransactionState*>(state);
	if (transaction == nullptr) return;
	transaction->fex.Commit(transaction->range_base,
	                        transaction->range_size);
	state = nullptr;
	delete transaction;
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64
	auto* transaction = static_cast<WindowsMutationTransactionState*>(state);
	if (transaction == nullptr) return;
	state = nullptr;
	delete transaction;
#else
	// Sparse reservation transactions are not implemented by this backend.
	state = nullptr;
#endif
}

bool MutationTransaction::Abort()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	auto* transaction = static_cast<AndroidMutationTransactionState*>(state);
	if (transaction == nullptr) return true;
	const bool result = AbortAndroidTransaction(*transaction);
	state = nullptr;
	delete transaction;
	return result;
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64
	auto* transaction = static_cast<WindowsMutationTransactionState*>(state);
	if (transaction == nullptr) return true;
	const bool result = AbortWindowsTransaction(*transaction);
	state = nullptr;
	delete transaction;
	return result;
#else
	const bool inactive = state == nullptr;
	state = nullptr;
	return inactive;
#endif
}

bool BeginReserveAligned(uint64_t address, uint64_t size, uint64_t alignment,
	                     ReservationHandle* handle,
	                     MutationTransaction* transaction,
	                     ReservationPlacement placement,
	                     AllocationDenialMetadata* denial)
{
	// Platforms without a qualified admission conflict never authorize retry.
	if (denial != nullptr) *denial = {AllocationFailure::HostAllocationFailed, false};
	if (handle != nullptr) *handle = {};

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	uint64_t requested_end = 0;
	const uint64_t host_page_size = AndroidHostPageSize();
	const bool valid_placement = placement == ReservationPlacement::Exact ||
	                             placement == ReservationPlacement::PreferAddress;
	const bool supported_requested_range =
	    address == 0 ||
	    AndroidGuestRangeContains(address, size, ANDROID_GUEST_SYSTEM_MIN,
	                              ANDROID_GUEST_SYSTEM_MAX) ||
	    AndroidGuestRangeContains(address, size, ANDROID_GUEST_USER_MIN,
	                              ANDROID_GUEST_USER_MAX);
	if (handle == nullptr || transaction == nullptr || transaction->IsActive() ||
	    host_page_size == 0 || !AndroidRangeEnd(address, size, &requested_end) ||
	    size > (std::numeric_limits<size_t>::max)() ||
	    address > (std::numeric_limits<uintptr_t>::max)() ||
	    alignment > (std::numeric_limits<uintptr_t>::max)() ||
	    !AndroidIsPowerOfTwo(alignment) || alignment < host_page_size ||
	    size % host_page_size != 0 ||
	    (address != 0 && address % alignment != 0) || !valid_placement ||
	    (!supported_requested_range &&
	     placement == ReservationPlacement::Exact))
	{
		if (denial != nullptr) denial->reason = AllocationFailure::InvalidArgument;
		return false;
	}

	auto& registry = AndroidRegistry();
	auto* pending =
	    new (std::nothrow) AndroidMutationTransactionState(
	        registry, FexGuestMemoryMutationKind::AdditiveMapping);
	if (pending == nullptr) return false;
	if (!pending->IsAdmitted())
	{
		if (denial != nullptr && !pending->fex.IsAdmitted())
			*denial = {AllocationFailure::AdmissionDenied, pending->fex.IsRetryable()};
		delete pending;
		return false;
	}
	if (!EnsureAndroidRegistryCapacity(
	        &registry.reservations, registry.reservation_num,
	        &registry.reservation_capacity) ||
	    registry.next_reservation_identity == 0)
	{
		delete pending;
		return false;
	}

	uint64_t allocation_address = address;
	const bool preferred_unavailable =
	    address != 0 &&
	    (!supported_requested_range ||
	     AnyAndroidReservationOverlap(registry, address, size) ||
	     AnyAndroidMappingOverlap(registry, address, size));
	if (preferred_unavailable && placement == ReservationPlacement::Exact)
	{
		delete pending;
		return false;
	}
	if (preferred_unavailable) allocation_address = 0;

	const uint64_t base = AndroidReserveHost(
	    allocation_address, size, alignment, placement);
	if (base == 0 || AnyAndroidReservationOverlap(registry, base, size) ||
	    AnyAndroidMappingOverlap(registry, base, size))
	{
		if (base != 0)
		{
			(void)munmap(
			    reinterpret_cast<void*>(static_cast<uintptr_t>(base)),
			    static_cast<size_t>(size));
		}
		delete pending;
		return false;
	}

	const AndroidReservation reservation {
	    base, size, registry.next_reservation_identity++};
	registry.reservations[registry.reservation_num++] = reservation;
	pending->kind = AndroidTransactionKind::Reservation;
	pending->reservation = ReservationHandle {
	    reservation.base, reservation.size, reservation.identity};
	pending->range_base = reservation.base;
	pending->range_size = reservation.size;
	*handle = pending->reservation;
	InvalidateAndroidRangeSeals(reservation.base, reservation.size);
	RecordMutation();
	transaction->state = pending;
	if (denial != nullptr) *denial = {};
	return true;
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64
	uint64_t requested_end = 0;
	const uint64_t host_page_size = WindowsHostPageSize();
	const bool valid_placement = placement == ReservationPlacement::Exact ||
	                             placement == ReservationPlacement::PreferAddress;
	if (handle == nullptr || transaction == nullptr || transaction->IsActive() ||
	    !WindowsRangeEnd(address, size, &requested_end) ||
	    alignment == 0 || (alignment & (alignment - 1)) != 0 ||
	    alignment < host_page_size || size % host_page_size != 0 ||
	    (address != 0 && address % alignment != 0) || !valid_placement)
	{
		if (denial != nullptr) denial->reason = AllocationFailure::InvalidArgument;
		return false;
	}

	auto& registry = WindowsRegistry();
	auto* pending  = new (std::nothrow) WindowsMutationTransactionState(registry);
	if (pending == nullptr) return false;
	if (!EnsureWindowsRegistryCapacity(&registry.reservations, registry.reservation_num,
	                                   &registry.reservation_capacity) ||
	    registry.next_reservation_identity == 0)
	{
		delete pending;
		return false;
	}

	const uint64_t base = WindowsReserveHost(address, size, alignment, placement);
	if (base == 0 || AnyWindowsReservationOverlap(registry, base, size) || AnyWindowsMappingOverlap(registry, base, size))
	{
		if (base != 0)
			(void)VirtualFree(reinterpret_cast<void*>(static_cast<uintptr_t>(base)), 0, MEM_RELEASE);
		delete pending;
		return false;
	}

	const WindowsReservation reservation {base, size, registry.next_reservation_identity++};
	registry.reservations[registry.reservation_num++] = reservation;
	pending->kind        = WindowsTransactionKind::Reservation;
	pending->reservation = ReservationHandle {reservation.base, reservation.size, reservation.identity};
	*handle              = pending->reservation;
	RecordMutation();
	transaction->state = pending;
	if (denial != nullptr) *denial = {};
	return true;
#else
	(void)address;
	(void)size;
	(void)alignment;
	(void)transaction;
	(void)placement;
	return false;
#endif
}

ReservedFixedResult BeginAllocFixedInReservation(
	uint64_t address, uint64_t size, Mode mode, MutationTransaction* transaction)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (transaction == nullptr || transaction->IsActive() ||
	    !AndroidValidMode(mode))
	{
		return ReservedFixedResult::Failed;
	}
	auto& registry = AndroidRegistry();
	{
		std::lock_guard<std::mutex> lock(registry.mutex);
		if (!AnyAndroidReservationOverlap(registry, address, size))
		{
			return ReservedFixedResult::NotReserved;
		}
	}

	uint64_t requested_end = 0;
	const uint64_t host_page_size = AndroidHostPageSize();
	if (host_page_size == 0 ||
	    !AndroidRangeEnd(address, size, &requested_end) ||
	    size > (std::numeric_limits<size_t>::max)() ||
	    address > (std::numeric_limits<uintptr_t>::max)() ||
	    address % host_page_size != 0 || size % host_page_size != 0)
	{
		return ReservedFixedResult::Failed;
	}
	const uint64_t page_count = size / ANDROID_GUEST_PAGE_SIZE;
	if (page_count == 0 ||
	    page_count > (std::numeric_limits<size_t>::max)() / sizeof(Mode))
	{
		return ReservedFixedResult::Failed;
	}
	auto* page_modes = static_cast<Mode*>(
	    std::malloc(static_cast<size_t>(page_count) * sizeof(Mode)));
	if (page_modes == nullptr) return ReservedFixedResult::Failed;
	for (uint64_t page = 0; page < page_count; page++)
	{
		page_modes[static_cast<size_t>(page)] = mode;
	}

	auto* pending =
	    new (std::nothrow) AndroidMutationTransactionState(
	        registry, FexGuestMemoryMutationKind::Exclusive);
	if (pending == nullptr || !pending->IsAdmitted())
	{
		delete pending;
		std::free(page_modes);
		return ReservedFixedResult::Failed;
	}
	AndroidReservation* reservation =
	    FindAndroidContainingReservation(registry, address, size);
	if (reservation == nullptr ||
	    AnyAndroidMappingOverlap(registry, address, size) ||
	    !EnsureAndroidRegistryCapacity(
	        &registry.mappings, registry.mapping_num,
	        &registry.mapping_capacity) ||
	    registry.next_mapping_identity == 0 ||
	    !AndroidActivateReservationRange(address, size, mode))
	{
		delete pending;
		std::free(page_modes);
		return ReservedFixedResult::Failed;
	}

	AndroidReservedMapping mapping {};
	mapping.base = address;
	mapping.size = size;
	mapping.identity = registry.next_mapping_identity++;
	mapping.reservation_identity = reservation->identity;
	mapping.page_modes = page_modes;
	mapping.page_mode_num = page_count;
	registry.mappings[registry.mapping_num++] = mapping;
	pending->kind = AndroidTransactionKind::ReservedMapping;
	pending->mapping_identity = mapping.identity;
	pending->range_base = address;
	pending->range_size = size;
	RecordMutation();
	transaction->state = pending;
	return ReservedFixedResult::Begun;
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64
	uint64_t requested_end = 0;
	const uint64_t host_page_size = WindowsHostPageSize();
	if (transaction == nullptr || transaction->IsActive() || !WindowsValidMode(mode) ||
	    !WindowsRangeEnd(address, size, &requested_end) || address % host_page_size != 0 || size % host_page_size != 0)
	{
		return ReservedFixedResult::Failed;
	}

	auto& registry = WindowsRegistry();
	auto* pending  = new (std::nothrow) WindowsMutationTransactionState(registry);
	if (pending == nullptr) return ReservedFixedResult::Failed;
	if (!AnyWindowsReservationOverlap(registry, address, size))
	{
		delete pending;
		return ReservedFixedResult::NotReserved;
	}
	WindowsReservation* reservation = FindWindowsContainingReservation(registry, address, size);
	if (reservation == nullptr || AnyWindowsMappingOverlap(registry, address, size) ||
	    !EnsureWindowsRegistryCapacity(&registry.mappings, registry.mapping_num, &registry.mapping_capacity))
	{
		delete pending;
		return ReservedFixedResult::Failed;
	}

	auto* allocation = VirtualAlloc(reinterpret_cast<void*>(static_cast<uintptr_t>(address)),
	                                static_cast<SIZE_T>(size), MEM_COMMIT, WindowsProtection(mode));
	if (allocation == nullptr || reinterpret_cast<uint64_t>(allocation) != address)
	{
		if (allocation != nullptr)
		{
			(void)VirtualFree(allocation, static_cast<SIZE_T>(size), MEM_DECOMMIT);
		}
		delete pending;
		return ReservedFixedResult::Failed;
	}

	const WindowsReservedMapping mapping {address, size, reservation->identity};
	registry.mappings[registry.mapping_num++] = mapping;
	pending->kind    = WindowsTransactionKind::ReservedMapping;
	pending->mapping = mapping;
	RecordMutation();
	transaction->state = pending;
	return ReservedFixedResult::Begun;
#else
	(void)address;
	(void)size;
	(void)mode;
	return transaction != nullptr && !transaction->IsActive()
	           ? ReservedFixedResult::NotReserved
	           : ReservedFixedResult::Failed;
#endif
}

bool HasReservationOverlap(uint64_t address, uint64_t size)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	auto& registry = AndroidRegistry();
	std::lock_guard<std::mutex> lock(registry.mutex);
	return AnyAndroidReservationOverlap(registry, address, size);
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64
	auto& registry = WindowsRegistry();
	std::lock_guard<std::mutex> lock(registry.mutex);
	return AnyWindowsReservationOverlap(registry, address, size);
#else
	(void)address;
	(void)size;
	return false;
#endif
}

bool ReleaseReservation(const ReservationHandle& handle)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (!handle.IsValid()) return false;
	FexGuestMemoryMutation mutation {
	    FexGuestMemoryMutationKind::Exclusive};
	if (!mutation.IsAdmitted()) return false;
	auto& registry = AndroidRegistry();
	std::lock_guard<std::mutex> lock(registry.mutex);
	const size_t reservation_index =
	    FindAndroidReservationHandle(registry, handle);
	if (reservation_index >= registry.reservation_num) return false;
	InvalidateAndroidRangeSeals(handle.base, handle.size);
	if (munmap(reinterpret_cast<void*>(static_cast<uintptr_t>(handle.base)),
	           static_cast<size_t>(handle.size)) != 0)
	{
		return false;
	}
	for (size_t index = registry.mapping_num; index != 0; index--)
	{
		if (registry.mappings[index - 1].reservation_identity ==
		    handle.identity)
		{
			EraseAndroidMapping(registry, index - 1);
		}
	}
	EraseAndroidReservation(registry, reservation_index);
	RecordMutation();
	mutation.Commit(handle.base, handle.size);
	return true;
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64
	if (!handle.IsValid()) return false;
	auto& registry = WindowsRegistry();
	std::lock_guard<std::mutex> lock(registry.mutex);
	const size_t reservation_index = FindWindowsReservationHandle(registry, handle);
	if (reservation_index >= registry.reservation_num ||
	    VirtualFree(reinterpret_cast<void*>(static_cast<uintptr_t>(handle.base)), 0, MEM_RELEASE) == 0)
	{
		return false;
	}
	for (size_t index = registry.mapping_num; index != 0; index--)
	{
		if (registry.mappings[index - 1].reservation_identity == handle.identity)
			EraseWindowsMapping(registry, index - 1);
	}
	EraseWindowsReservation(registry, reservation_index);
	RecordMutation();
	return true;
#else
	(void)handle;
	return false;
#endif
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
bool IsImmutableRegionSnapshotEnabled()
{
	return ImmutableSnapshot::Requested();
}

bool RequiresUniformHostPageDataProtection()
{
	return sys_virtual_requires_uniform_host_page_data_protection();
}

ManagedRegionState QueryManagedRegion(uint64_t address, ManagedRegion* region)
{
	if (region != nullptr) *region = ManagedRegion {0, 0, Mode::NoAccess};
	bool handled = false;
	const ManagedRegionState reserved_state =
	    QueryAndroidReservedRegion(address, region, &handled);
	if (handled) return reserved_state;
	return sys_virtual_query_managed_region(address, region);
}

bool QueryGuestBackingSpan(uint64_t address, uint64_t maximum_bytes, GuestBackingSpan* span) noexcept
{
	if (span != nullptr) *span = {};
	address &= ANDROID_DATA_ADDRESS_MASK;
	if (span == nullptr || address == 0 || maximum_bytes == 0 || address > UINT64_MAX - maximum_bytes) return false;
	if (const auto query = g_backing_owner_query.load(std::memory_order_acquire);
	    query != nullptr && query(address, maximum_bytes, span)) return span->identity != 0 && span->size != 0;
	{
		auto& registry = AndroidRegistry();
		std::lock_guard lock(registry.mutex);
		const auto* mapping = FindAndroidContainingMapping(registry, address, 1);
		if (mapping != nullptr)
		{
			if (mapping->identity == 0 || mapping->identity >= (UINT64_C(1) << 63)) return false;
			const auto offset = address - mapping->base;
			*span = {mapping->identity | (UINT64_C(1) << 63), offset, address,
			    std::min(maximum_bytes, mapping->size - offset), GetMutationGeneration()};
			return true;
		}
		if (AnyAndroidReservationOverlap(registry, address, 1)) return false;
	}
	if (!sys_virtual_query_backing_span(address, maximum_bytes, span)) return false;
	span->generation = GetMutationGeneration();
	return true;
}

bool MarkGuestBackingHandoffRange(uint64_t address, uint64_t bytes, uint32_t watch_intents) noexcept
{
	address &= ANDROID_DATA_ADDRESS_MASK;
	if (address == 0 || bytes == 0 || address > UINT64_MAX - bytes || watch_intents < 1u || watch_intents > 3u) return false;
	if (NativeClaimCoverageRequired() && !CurrentNativeClaimCoversGuestRange(address, bytes)) return false;
	const auto mark = g_backing_owner_mark.load(std::memory_order_acquire);
	if (mark != nullptr && mark(address, bytes, watch_intents)) return true;
	const auto end = address + bytes;
	while (address < end)
	{
		GuestBackingSpan span;
		if (!QueryGuestBackingSpan(address, end - address, &span) || span.size == 0) return false;
		// Current Core allocations are private anonymous mappings; duplicate
		// direct-memory aliases are rejected by the PS4 VM owner. The backing
		// identity/offset is retained separately from this accepted VA.
		// A bulk plane may span several physical allocations. Let the owner
		// mark each span's aliases even when it could not cover the whole plane.
		if (mark == nullptr || !mark(span.address, span.size, watch_intents))
			GuestCpu::MarkHandoffRange(span.address, span.size, static_cast<GuestCpu::MemoryAccessIntent>(watch_intents));
		address += span.size;
	}
	return true;
}

bool IsManagedAddress(uint64_t address)
{
	bool handled = false;
	const bool reserved_managed =
	    IsAndroidReservedManagedAddress(address, &handled);
	if (handled) return reserved_managed;
	return sys_virtual_is_managed_address(address);
}
#else
bool QueryGuestBackingSpan(uint64_t address, uint64_t maximum_bytes, GuestBackingSpan* span) noexcept
{
	if (span != nullptr) *span = {};
	if (span == nullptr || address == 0 || maximum_bytes == 0 || address > UINT64_MAX - maximum_bytes) return false;
	const auto query = g_backing_owner_query.load(std::memory_order_acquire);
	return query != nullptr && query(address, maximum_bytes, span) && span->identity != 0 && span->size != 0;
}
bool MarkGuestBackingHandoffRange(uint64_t address, uint64_t bytes, uint32_t watch_intents) noexcept
{
	if (address == 0 || bytes == 0 || address > UINT64_MAX - bytes || watch_intents < 1u || watch_intents > 3u) return false;
	if (NativeClaimCoverageRequired() && !CurrentNativeClaimCoversGuestRange(address, bytes)) return false;
	const auto mark = g_backing_owner_mark.load(std::memory_order_acquire);
	return mark != nullptr && mark(address, bytes, watch_intents);
}
#endif

uint64_t CreateGuestBackingHandoffOwner() noexcept
{
	return GuestCpu::CreateMemoryHandoffOwner();
}

bool ReleaseGuestBackingHandoffOwner(uint64_t owner) noexcept
{
	return GuestCpu::ReleaseMemoryHandoffOwner(owner);
}

bool MarkGuestBackingHandoffRangeOwned(uint64_t owner, uint64_t address, uint64_t bytes, uint32_t watch_intents) noexcept
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	address &= UINT64_C(0x00ffffffffffffff);
#endif
	if (owner == 0 || address == 0 || bytes == 0 || address > UINT64_MAX - bytes ||
	    watch_intents < 1u || watch_intents > 3u) return false;
	if (NativeClaimCoverageRequired() && !CurrentNativeClaimCoversGuestRange(address, bytes)) return false;
	const auto owner_query = g_backing_owner_query.load(std::memory_order_acquire);
	const auto owner_mark = owner_query != nullptr ? g_backing_owner_mark_owned.load(std::memory_order_acquire) : nullptr;
	if (owner_mark != nullptr && owner_mark(owner, address, bytes, watch_intents)) return true;
	const auto end = address + bytes;
	while (address < end)
	{
		GuestBackingSpan span;
		if (!QueryGuestBackingSpan(address, end - address, &span) || span.identity == 0 ||
		    span.address != address || span.size == 0 || span.size > end - address ||
		    span.offset > UINT64_MAX - span.size) return false;
		GuestBackingSpan physical;
		if (owner_query != nullptr && owner_query(address, span.size, &physical))
		{
			// A physical owner must enumerate all aliases and remember future
			// remaps. Never silently turn an unavailable owned protocol into a
			// private-VA guard or a legacy sticky enrollment.
			if (owner_mark == nullptr || !owner_mark(owner, address, span.size, watch_intents)) return false;
		} else
		{
			// Core-private allocations have one accepted VA per identity. A
			// future allocation has a different identity even at the same VA.
			const GuestCpu::PhysicalMemoryRange range {span.identity, span.offset, span.size};
			if (!GuestCpu::EnrollMemoryHandoffPhysicalRange(owner, range,
			        static_cast<GuestCpu::MemoryAccessIntent>(watch_intents)) ||
			    !GuestCpu::MapMemoryHandoffOwnerPhysicalAlias(owner, range, span.address)) return false;
		}
		address += span.size;
	}
	return true;
}

bool CopyGuestBackingDataForGpu(uint64_t address, void* buffer, uint64_t bytes, bool write) noexcept
{
	if (address == 0 || buffer == nullptr || bytes == 0 || bytes > SIZE_MAX || address > UINT64_MAX - bytes) return false;
	if (NativeClaimCoverageRequired() && !CurrentNativeClaimCoversGuestRange(address, bytes)) return false;
	// Registration publishes the query last; never consume a partial owner.
	if (g_backing_owner_query.load(std::memory_order_acquire) != nullptr)
	{
		const auto copy = g_backing_owner_gpu_copy.load(std::memory_order_acquire);
		if (copy != nullptr && copy(address, buffer, bytes, write)) return true;
	}
	GuestCpu::MemoryHandoffBypass producer;
	GuestDataLease lease;
	if (!TryAcquireGuestDataLease(address, bytes, write ? Mode::Write : Mode::Read, &lease, false)) return false;
	auto* guest = reinterpret_cast<void*>(static_cast<uintptr_t>(lease.HostBegin()));
	if (write) std::memcpy(guest, buffer, static_cast<size_t>(bytes));
	else std::memcpy(buffer, guest, static_cast<size_t>(bytes));
	return true;
}

GuestBackingPresence QueryGuestBackingPresence(uint64_t identity, uint64_t offset, uint64_t bytes) noexcept
{
	if (identity == 0 || bytes == 0 || offset > UINT64_MAX - bytes) return GuestBackingPresence::Unknown;
	if (g_backing_owner_query.load(std::memory_order_acquire) != nullptr)
	{
		const auto presence = g_backing_owner_presence.load(std::memory_order_acquire);
		// An owner without this proof may emit identities in an unknown domain.
		if (presence == nullptr) return GuestBackingPresence::Unknown;
		const auto result = presence(identity, offset, bytes);
		if (result != GuestBackingPresence::Unknown) return result;
	}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if ((identity & (UINT64_C(1) << 63)) != 0)
	{
		const uint64_t mapping_identity = identity & ~(UINT64_C(1) << 63);
		auto& registry = AndroidRegistry();
		std::lock_guard lock(registry.mutex);
		if (mapping_identity == 0 || mapping_identity >= registry.next_mapping_identity)
			return GuestBackingPresence::Unknown;
		for (size_t index = 0; index != registry.mapping_num; ++index)
		{
			const auto& mapping = registry.mappings[index];
			if (mapping.identity == mapping_identity && offset < mapping.size)
				return GuestBackingPresence::Live;
		}
		return GuestBackingPresence::Retired;
	}
	return sys_virtual_query_backing_presence(identity, offset, bytes);
#else
	return GuestBackingPresence::Unknown;
#endif
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
bool BindFreshGuestMappingToSharedBacking(uint64_t address, uint64_t bytes, int fd,
    uint64_t backing_offset, Mode mode) noexcept
{
	const uint64_t host_page = AndroidHostPageSize();
	if (host_page == 0 || fd < 0 || address == 0 || bytes == 0 || bytes > SIZE_MAX ||
	    address > UINT64_MAX - bytes || backing_offset > UINT64_MAX - bytes ||
	    backing_offset > static_cast<uint64_t>((std::numeric_limits<off_t>::max)()) ||
	    address % host_page != 0 || bytes % host_page != 0 || backing_offset % host_page != 0 ||
	    !AndroidValidMode(mode)) return false;
	struct stat file {};
	if (fstat(fd, &file) != 0 || file.st_size < 0 || backing_offset + bytes > static_cast<uint64_t>(file.st_size)) return false;
	FexGuestMemoryMutation mutation {FexGuestMemoryMutationKind::AdditiveMapping};
	if (!mutation.IsAdmitted()) return false;
	bool reserved_mapping = false;
	uint64_t reserved_identity = 0;
	{
		auto& registry = AndroidRegistry();
		auto lock = LockAndroidRegistryForQuery(registry);
		const auto* mapping = FindAndroidContainingMapping(registry, address, bytes);
		if (mapping != nullptr)
		{
			if (mapping->base != address || mapping->size != bytes) return false;
			for (uint64_t page = 0; page != mapping->page_mode_num; ++page)
				if (mapping->page_modes[static_cast<size_t>(page)] != mode) return false;
			reserved_mapping = true;
			reserved_identity = mapping->identity;
		} else if (AnyAndroidReservationOverlap(registry, address, bytes)) return false;
	}
	if (!reserved_mapping)
	{
		uint64_t allocation_begin = 0, allocation_size = 0;
		if (!sys_virtual_query_allocation_range(address, &allocation_begin, &allocation_size) ||
		    allocation_begin != address || allocation_size != bytes) return false;
		uint64_t current = address;
		while (current < address + bytes)
		{
			ManagedRegion region {};
			if (sys_virtual_query_managed_region(current, &region) != ManagedRegionState::Visible ||
			    region.begin > current || region.end <= current || region.mode != mode) return false;
			current = std::min(address + bytes, region.end);
		}
	}
	// Only the caller's fresh unpublished extent is replaced. Every failure
	// restores its original anonymous mapping with exactly the requested mode;
	// the owner then aborts/frees that extent before exposing any address.
	InvalidateAndroidRangeSeals(address, bytes);
	void* const requested = reinterpret_cast<void*>(static_cast<uintptr_t>(address));
	void* const mapped = mmap(requested, static_cast<size_t>(bytes), AndroidHostProtection(mode),
	    MAP_SHARED | MAP_FIXED, fd, static_cast<off_t>(backing_offset));
	if (mapped != requested)
	{
		if (mapped != MAP_FAILED) munmap(mapped, static_cast<size_t>(bytes));
		if (mmap(requested, static_cast<size_t>(bytes), AndroidHostProtection(mode),
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != requested) std::abort();
		return false;
	}
	if (reserved_mapping)
	{
		auto& registry = AndroidRegistry();
		auto lock = LockAndroidRegistryForQuery(registry);
		const auto index = FindAndroidMappingIdentity(registry, reserved_identity);
		if (index >= registry.mapping_num) std::abort();
		registry.mappings[index].shared_backing_fd = fd;
		registry.mappings[index].shared_backing_offset = backing_offset;
	}
	RecordMutation();
	mutation.Commit(address, bytes);
	return true;
}
#endif

uint64_t Alloc(uint64_t address, uint64_t size, Mode mode, AllocationDenialMetadata* denial)
{
	if (denial != nullptr) *denial = {};
	if (size == 0 || size > SIZE_MAX || size > UINT64_MAX - address ||
	    static_cast<uint32_t>(mode) > static_cast<uint32_t>(Mode::ExecuteReadWrite))
	{
		if (denial != nullptr) denial->reason = AllocationFailure::InvalidArgument;
		return 0;
	}
	FexGuestMemoryMutation mutation {
	    FexGuestMemoryMutationKind::AdditiveMapping};
	if (!mutation.IsAdmitted())
	{
		if (denial != nullptr) *denial = {AllocationFailure::AdmissionDenied, mutation.IsRetryable()};
		return 0;
	}
	const uint64_t result = sys_virtual_alloc(address, size, mode);
	if (result != 0)
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		InvalidateAndroidRangeSeals(result, size);
#endif
		RecordMutation();
		mutation.Commit(result, size);
	}
	else if (denial != nullptr)
	{
		// Backends without typed admission (including Darwin's local VM gate)
		// supply only an allocation API failure, not proof of physical OOM.
		denial->reason = AllocationFailure::HostAllocationFailed;
	}
	return result;
}

uint64_t AllocAligned(uint64_t address, uint64_t size, Mode mode, uint64_t alignment)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (static_cast<uint32_t>(mode) > static_cast<uint32_t>(Mode::ExecuteReadWrite)) return 0;
#endif
	FexGuestMemoryMutation mutation {
	    FexGuestMemoryMutationKind::AdditiveMapping};
	if (!mutation.IsAdmitted()) return 0;
	const uint64_t result = sys_virtual_alloc_aligned(address, size, mode, alignment);
	if (result != 0)
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		InvalidateAndroidRangeSeals(result, size);
#endif
		RecordMutation();
		mutation.Commit(result, size);
	}
	return result;
}

bool AllocFixed(uint64_t address, uint64_t size, Mode mode)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (static_cast<uint32_t>(mode) > static_cast<uint32_t>(Mode::ExecuteReadWrite)) return false;
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	MutationTransaction reserved_transaction;
	const ReservedFixedResult reserved_result =
	    BeginAllocFixedInReservation(address, size, mode, &reserved_transaction);
	if (reserved_result == ReservedFixedResult::Begun)
	{
		reserved_transaction.Commit();
		return true;
	}
	if (reserved_result == ReservedFixedResult::Failed) return false;
#endif
	FexGuestMemoryMutation mutation {
	    FexGuestMemoryMutationKind::AdditiveMapping};
	if (!mutation.IsAdmitted()) return false;
	const bool result = sys_virtual_alloc_fixed(address, size, mode);
	if (result)
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		InvalidateAndroidRangeSeals(address, size);
#endif
		RecordMutation();
		constexpr uint64_t tracked_page_size = 4096;
		const uint64_t first_page = address & ~(tracked_page_size - 1);
		const uint64_t last_page = (address + size - 1) &
		                           ~(tracked_page_size - 1);
		mutation.Commit(first_page,
		                last_page - first_page + tracked_page_size);
	}
	return result;
}

bool Free(uint64_t address)
{
	FexGuestMemoryMutation mutation {
	    FexGuestMemoryMutationKind::Exclusive};
	if (!mutation.IsAdmitted()) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	uint64_t released_size = 0;
	const auto reserved_result =
	    FreeAndroidReservedMapping(address, &released_size);
	if (reserved_result == AndroidReservedFreeResult::Released)
	{
		mutation.Commit(address, released_size);
		return true;
	}
	if (reserved_result == AndroidReservedFreeResult::Failed) return false;
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64
	uint64_t released_size = 0;
	const auto reserved_result = FreeWindowsReservedMapping(address, &released_size);
	if (reserved_result == WindowsReservedFreeResult::Released)
	{
		mutation.Commit(address, released_size);
		return true;
	}
	if (reserved_result == WindowsReservedFreeResult::Failed) return false;
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	uint64_t freed_begin = 0, freed_size = 0;
	const bool allocation_found =
	    sys_virtual_query_allocation_range(address, &freed_begin, &freed_size);
	// The Android allocation registry stores the complete original extent,
	// independently of protection runs. The exclusive epoch covers both this
	// lookup and free; competing mapping mutations cannot replace its owner.
	const bool exact_freed_range = allocation_found && freed_begin != 0 &&
	    freed_size != 0 && freed_begin == (address & ~UINT64_C(0xfff)) &&
	    freed_size <= (std::numeric_limits<uint64_t>::max)() - freed_begin;
	if (allocation_found)
		InvalidateAndroidRangeSeals(freed_begin, freed_size);
	else
		InvalidateAndroidRangeSeals(0, 0);
#endif
	const bool result = sys_virtual_free(address);
	if (result)
	{
		RecordMutation();
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
		// Keep lifetime-watch revocation tied to the allocation even when its
		// bounds cannot justify limiting translation-cache invalidation.
		mutation.Commit(exact_freed_range ? freed_begin : 0,
		                exact_freed_range ? freed_size : (std::numeric_limits<uint64_t>::max)(),
		                freed_begin, freed_size);
#else
		// Backends without a complete allocation query invalidate every
		// protection run conservatively.
		mutation.Commit(0, (std::numeric_limits<uint64_t>::max)());
#endif
	}
	return result;
}

bool Protect(uint64_t address, uint64_t size, Mode mode, Mode* old_mode)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (static_cast<uint32_t>(mode) > static_cast<uint32_t>(Mode::ExecuteReadWrite)) return false;
#endif
	FexGuestMemoryMutation mutation {
	    FexGuestMemoryMutationKind::Exclusive};
	if (!mutation.IsAdmitted()) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	const auto reserved_result =
	    ProtectAndroidReservedMapping(address, size, mode, old_mode);
	if (reserved_result == AndroidReservedProtectResult::Protected)
	{
		RecordMutation();
		mutation.Commit(address, size);
		return true;
	}
	if (reserved_result == AndroidReservedProtectResult::Failed) return false;
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64
	const auto reserved_result = ProtectWindowsReservedMapping(address, size, mode, old_mode);
	if (reserved_result == WindowsReservedProtectResult::Protected)
	{
		RecordMutation();
		mutation.Commit(address, size);
		return true;
	}
	if (reserved_result == WindowsReservedProtectResult::Failed) return false;
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	InvalidateAndroidRangeSeals(address, size);
#endif
	const bool result = sys_virtual_protect(address, size, mode, old_mode);
	if (result)
	{
		RecordMutation();
		mutation.Commit(address, size);
	}
	return result;
}

bool FlushInstructionCache(uint64_t address, uint64_t size)
{
	FexGuestMemoryMutation mutation {
	    FexGuestMemoryMutationKind::Exclusive};
	if (!mutation.IsAdmitted()) return false;
	const bool result = sys_virtual_flush_instruction_cache(address, size);
	// This call is also the publication point for bytes written through a
	// non-executable mapping. Invalidate conservatively even if the platform
	// cache operation reports failure; the bytes may already have changed.
	RecordMutation();
	mutation.Commit(address, size);
	return result;
}

bool PatchReplace(uint64_t vaddr, uint64_t value)
{
	FexGuestMemoryMutation mutation {
	    FexGuestMemoryMutationKind::Exclusive};
	if (!mutation.IsAdmitted()) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	InvalidateAndroidRangeSeals(vaddr, sizeof(value));
#endif
	const bool result = sys_virtual_patch_replace(vaddr, value);
	// false also covers an unchanged value and failures after the store (for
	// example, restoring RX protection). Both require a conservative publish so
	// no already-translated block can survive a partial platform operation.
	RecordMutation();
	mutation.Commit(result ? vaddr : 0,
	                result ? sizeof(value)
	                       : (std::numeric_limits<uint64_t>::max)());
	return result;
}

} // namespace VirtualMemory

} // namespace Lumen::Core
