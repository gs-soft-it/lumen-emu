#include "Emulator/Libs/NpParental.h"

#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/NpRequests.h"
#include "Emulator/Loader/RuntimeLinker.h"

#include <array>
#include <cstdio>
#include <cstdlib>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::NpParental {

LIB_NAME("NpManager", "NpManager");

namespace {

const char* ResultName(ModelResult result)
{
	switch (result)
	{
		case ModelResult::Ok: return "complete-restricted";
		case ModelResult::Disabled: return "disabled";
		case ModelResult::InvalidRequest: return "request-not-live";
		case ModelResult::InvalidIdentifier: return "identifier-unreadable";
		case ModelResult::InvalidAgeOutput: return "age-unwritable";
		case ModelResult::InvalidRestrictionsOutput: return "restrictions-unwritable";
		case ModelResult::OverlappingOutputs: return "outputs-overlap";
	}
	return "unknown";
}

} // namespace

bool ExperimentalEnabled()
{
	return NpRequests::Enabled(NpRequests::Service::Parental);
}

ModelResult QueryModel(int32_t request_id, uint64_t identifier_address, uint64_t age_address, uint64_t restrictions_address)
{
#if !(defined(__aarch64__) || \
      (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64))
	(void)request_id;
	(void)identifier_address;
	(void)age_address;
	(void)restrictions_address;
	return ModelResult::Disabled;
#else
	if (!ExperimentalEnabled()) return ModelResult::Disabled;
	NpRequests::LiveRequestGuard request(request_id);
	if (!request.IsLive()) return ModelResult::InvalidRequest;

	// The experiment admits an opaque 20-byte identifier format. Snapshot it
	// before publishing outputs, including when the caller aliases this input.
	std::array<uint8_t, 20> identifier {};
	if (!Loader::ReadGuestMemory(identifier_address, identifier.data(), identifier.size()))
		return ModelResult::InvalidIdentifier;

	// This synthetic restricted profile asserts no real account age or rights.
	// One age byte and three Boolean bytes are the complete local footprints.
	constexpr uint8_t age = 0;
	constexpr std::array<uint8_t, 3> restrictions {1, 1, 1};
	if (!Loader::ValidateGuestMemory(age_address, sizeof(age), true)) return ModelResult::InvalidAgeOutput;
	if (!Loader::ValidateGuestMemory(restrictions_address, restrictions.size(), true))
		return ModelResult::InvalidRestrictionsOutput;
	if (age_address >= restrictions_address && age_address - restrictions_address < restrictions.size())
		return ModelResult::OverlappingOutputs;

	// Validate both outputs before either write. As with the existing Loader
	// helpers, callers must keep mappings and buffer ownership stable throughout.
	if (!Loader::WriteGuestMemory(age_address, &age, sizeof(age))) return ModelResult::InvalidAgeOutput;
	if (!Loader::WriteGuestMemory(restrictions_address, restrictions.data(), restrictions.size()))
		return ModelResult::InvalidRestrictionsOutput;
	return ModelResult::Ok;
#endif
}

int32_t LUMEN_SYSV_ABI GetParentalControlInfo(int32_t request_id, const void* identifier, void* age, void* restrictions)
{
	PRINT_NAME();
	const auto outcome = QueryModel(request_id, reinterpret_cast<uint64_t>(identifier),
	                                reinterpret_cast<uint64_t>(age), reinterpret_cast<uint64_t>(restrictions));
	std::fprintf(stderr,
	             "[NP_PARENTAL_OFFLINE_EXPERIMENT] operation=query request=%d result=%s "
	             "experimental=1 console_contract=unverified policy=restricted-local-profile\n",
	             request_id, ResultName(outcome));
	std::fflush(stderr);
	if (outcome != ModelResult::Ok)
	{
		EXIT("NP parental offline experiment cannot model query: %s", ResultName(outcome));
		std::abort();
	}
	return 0;
}

} // namespace Lumen::Libs::NpParental

#endif // LUMEN_EMU_ENABLED
