#include "Emulator/Libs/NpPlus.h"

#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/NpRequests.h"
#include "Emulator/Loader/RuntimeLinker.h"

#include <array>
#include <cstdio>
#include <cstdlib>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::NpPlus {

LIB_NAME("NpManager", "NpManager");

namespace {

const char* ResultName(ModelResult result)
{
	switch (result)
	{
		case ModelResult::Ok: return "complete-denied";
		case ModelResult::Disabled: return "disabled";
		case ModelResult::InvalidRequest: return "request-not-live";
		case ModelResult::InvalidDescriptor: return "descriptor-unreadable";
		case ModelResult::UnsupportedFormat: return "descriptor-format-unsupported";
		case ModelResult::InvalidOutput: return "result-unwritable";
	}
	return "unknown";
}

} // namespace

bool ExperimentalEnabled()
{
	return NpRequests::Enabled(NpRequests::Service::Plus);
}

ModelResult CheckModel(int32_t request_id, uint64_t descriptor_address, uint64_t result_address)
{
#if !(defined(__aarch64__) || \
      (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64))
	// The checked guest-memory helpers are available only on active native/FEX runtimes.
	(void)request_id;
	(void)descriptor_address;
	(void)result_address;
	return ModelResult::Disabled;
#else
	if (!ExperimentalEnabled()) return ModelResult::Disabled;
	// Serialize query completion with deletion/reset, including the output write.
	// No request can be invalidated while this query is still publishing a result.
	NpRequests::LiveRequestGuard request(request_id);
	if (!request.IsLive()) return ModelResult::InvalidRequest;

	// This is the explicitly chosen 56-byte diagnostic format. Read a stable
	// snapshot so an overlapping result cannot alter validation halfway through.
	std::array<uint8_t, 56> descriptor {};
	if (!Loader::ReadGuestMemory(descriptor_address, descriptor.data(), descriptor.size()))
		return ModelResult::InvalidDescriptor;
	uint64_t declared_size = 0;
	for (size_t byte = 0; byte < sizeof(declared_size); ++byte)
		declared_size |= uint64_t(descriptor[byte]) << (byte * 8);
	if (declared_size != descriptor.size()) return ModelResult::UnsupportedFormat;

	// User/features data is opaque to this local denial policy. It grants no
	// entitlement and makes no network request. Only the authorization byte is
	// written; no console result-record size or reserved-byte policy is assumed.
	constexpr uint8_t denied = 0;
	if (!Loader::ValidateGuestMemory(result_address, sizeof(denied), true) ||
	    !Loader::WriteGuestMemory(result_address, &denied, sizeof(denied)))
		return ModelResult::InvalidOutput;
	return ModelResult::Ok;
#endif
}

int32_t LUMEN_SYSV_ABI CheckPlus(int32_t request_id, const void* descriptor, void* result)
{
	PRINT_NAME();
	const auto outcome = CheckModel(request_id, reinterpret_cast<uint64_t>(descriptor), reinterpret_cast<uint64_t>(result));
	std::fprintf(stderr,
	             "[NP_PLUS_OFFLINE_EXPERIMENT] operation=check request=%d result=%s "
	             "experimental=1 console_contract=unverified policy=deny-all\n",
	             request_id, ResultName(outcome));
	std::fflush(stderr);
	if (outcome != ModelResult::Ok)
	{
		EXIT("NP Plus offline experiment cannot model check: %s", ResultName(outcome));
		std::abort();
	}
	return 0;
}

} // namespace Lumen::Libs::NpPlus

#endif // LUMEN_EMU_ENABLED
