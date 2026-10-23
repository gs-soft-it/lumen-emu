#include "Lumen/Core/Threads.h"

#include "Emulator/Config.h"
#include "Emulator/Log.h"
#include "Emulator/Network.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

namespace {

namespace NetCtl = Lumen::Libs::Network::NetCtl;
using Query = int (LUMEN_SYSV_ABI *)(int*);
static_assert(std::is_same_v<decltype(&NetCtl::NetCtlGetState), Query>);
static_assert(sizeof(int) == 4 && std::is_signed_v<int>);

constexpr char SELECTOR[] = "LUMEN_EXPERIMENTAL_NETCTL_OFFLINE";
constexpr uint32_t CANARY = 0x6b95a73cu;

struct GuardedState
{
	uint32_t before;
	int state;
	uint32_t after;
};
static_assert(offsetof(GuardedState, state) == 4 && offsetof(GuardedState, after) == 8);
static_assert(sizeof(GuardedState) == 12);

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "NetCtlStateTest: %s\n", message);
	return condition;
}

} // namespace

int main(int argc, char** argv)
{
	if (argc != 2)
	{
		std::fputs("Usage: NetCtlStateTest <unset|zero|one|malformed>\n", stderr);
		return 2;
	}
	// Original fixtures for Lumen's selected local snapshot, not host network
	// connectivity, NP authentication, callbacks, or invalid-pointer behavior.
	constexpr struct Scenario { const char* name; const char* value; int state; } scenarios[] {
	    {"unset", nullptr, 3}, {"zero", "0", 3}, {"one", "1", 0}, {"malformed", "01", 3}};
	const Scenario* selected = nullptr;
	for (const auto& scenario: scenarios)
	{
		if (std::strcmp(argv[1], scenario.name) == 0) selected = &scenario;
	}
	if (!Check(selected != nullptr, "unknown selector scenario")) return 1;
	const char* value = std::getenv(SELECTOR);
	if (!Check(selected->value == nullptr ? value == nullptr : value != nullptr && std::strcmp(value, selected->value) == 0,
	           "scenario has the wrong environment")) return 1;

	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	for (int iteration = 0; iteration < 8; ++iteration)
	{
		GuardedState output {CANARY, -0x13579 - iteration, CANARY};
		if (!Check(NetCtl::NetCtlGetState(&output.state) == 0, "state query did not return success") ||
		    !Check(output.state == selected->state, "state query did not preserve the selected snapshot") ||
		    !Check(output.before == CANARY && output.after == CANARY, "state query wrote outside its four-byte output")) return 1;
	}
	std::puts("NetCtlStateTest: PASS");
	return 0;
}
