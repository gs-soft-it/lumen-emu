// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/emulator_settings.h"
#include "core/libraries/system/userservice.h"
#include "core/libraries/system/userservice_error.h"
#include "core/linker.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <vector>
#include <unistd.h>

#ifndef LUMEN_GEN4_DIRECT_FEX
#error This contract exercises the extracted UserService callback dispatch.
#endif

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); std::abort(); } } while (0)

namespace Core {
// Reuse the existing metadata fixture access without loading a guest image.
class PthreadWaitContractTest final {
public:
    static void InitializeBootMetadata() {
        auto& info = Common::ElfInfo::Instance();
        info.sdk_ver = Common::ElfInfo::FW_900;
        info.initialized = true;
    }
};
} // namespace Core

namespace {
using namespace Libraries::UserService;
std::vector<OrbisUserServiceEvent> received;

void PS4_SYSV_ABI Record(OrbisUserServiceEvent* event) {
    CHECK(event != nullptr);
    received.push_back(*event);
    // Native callbacks receive a local copy, never the queued event itself.
    event->userId = -1234;
}

void PS4_SYSV_ABI UnregisterWithinCallback(OrbisUserServiceEvent* event) {
    received.push_back(*event);
    CHECK(sceUserServiceUnregisterEventCallback(UnregisterWithinCallback) == 0);
}
} // namespace

int main() {
    alarm(10);
    const auto path = std::filesystem::temp_directory_path() /
        ("lumen-userservice-contract-" + std::to_string(getpid()));
    Common::FS::InitializeUserPaths(path);
    EmulatorSettingsImpl::SetInstance(std::make_shared<EmulatorSettingsImpl>());
    Common::Log::Setup("userservice-contract.log");
    Core::PthreadWaitContractTest::InitializeBootMetadata();
    // An empty production Linker classifies these native callbacks as host
    // functions. No guest module, translated runtime or VM mapping is started.
    Common::Singleton<Core::Linker>::Instance();
    CHECK(sceUserServiceRegisterEventCallback(nullptr) == ORBIS_USER_SERVICE_ERROR_INVALID_ARGUMENT);
    AddUserServiceEvent({OrbisUserServiceEventType::Login, 11});
    AddUserServiceEvent({OrbisUserServiceEventType::Logout, 22});
    CHECK(received.empty());
    CHECK(sceUserServiceRegisterEventCallback(Record) == 0);
    CHECK(received.size() == 2);
    CHECK(received[0].event == OrbisUserServiceEventType::Login && received[0].userId == 11);
    CHECK(received[1].event == OrbisUserServiceEventType::Logout && received[1].userId == 22);
    CHECK(sceUserServiceUnregisterEventCallback(UnregisterWithinCallback) == ORBIS_USER_SERVICE_ERROR_INVALID_ARGUMENT);
    AddUserServiceEvent({OrbisUserServiceEventType::Login, 33});
    CHECK(received.size() == 3 && received.back().userId == 33);
    CHECK(sceUserServiceUnregisterEventCallback(Record) == 0);
    received.clear();
    CHECK(sceUserServiceRegisterEventCallback(Record) == 0);
    CHECK(received.size() == 3);
    CHECK(received[0].userId == 11 && received[1].userId == 22 && received[2].userId == 33);
    CHECK(sceUserServiceUnregisterEventCallback(nullptr) == 0);
    received.clear();
    // Dispatch releases the registry mutex before calling user code. Clearing
    // the callback during the first replay suppresses the remaining dispatches.
    CHECK(sceUserServiceRegisterEventCallback(UnregisterWithinCallback) == 0);
    CHECK(received.size() == 1 && received[0].userId == 11);
    AddUserServiceEvent({OrbisUserServiceEventType::Logout, 44});
    CHECK(received.size() == 1);
    CHECK(sceUserServiceUnregisterEventCallback(nullptr) == 0);
    alarm(0);
    std::puts("direct-userservice=PASS native-callback/replay/local-copy/reentrant-unregister; guest-slot allocation and translated callback execution excluded");
}
