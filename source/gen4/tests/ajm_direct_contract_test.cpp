// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "core/libraries/ajm/ajm.h"
#include "core/libraries/ajm/ajm_error.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <unistd.h>

#ifndef LUMEN_GEN4_DIRECT_FEX
#error This contract exercises the extracted AJM implementation.
#endif

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); std::abort(); } } while (0)

namespace {
using namespace Libraries::Ajm;

struct ErrorSentinel {
    AjmBatchError value;
    std::array<unsigned char, sizeof(AjmBatchError)> original;
    ErrorSentinel() {
        std::memset(&value, 0xa5, sizeof(value));
        std::memcpy(original.data(), &value, sizeof(value));
    }
    void CheckUnchanged() const {
        CHECK(std::memcmp(&value, original.data(), sizeof(value)) == 0);
    }
};

void CheckEmptyBatch(u32 context, bool cancel) {
    // Priority and batch_error are ignored in the extracted implementation.
    ErrorSentinel error;
    u32 batch{};
    CHECK(sceAjmBatchStartBuffer(context, nullptr, 0, -12345, &error.value, &batch) == 0);
    error.CheckUnchanged();
    if (cancel) {
        CHECK(sceAjmBatchCancel(context, batch) == 0);
        CHECK(sceAjmBatchCancel(context, batch) == 0);
    }
    CHECK(sceAjmBatchWait(context, batch, 0, &error.value) ==
          (cancel ? ORBIS_AJM_ERROR_CANCELLED : 0));
    error.CheckUnchanged();
    CHECK(sceAjmBatchWait(context, batch, 0, &error.value) == ORBIS_AJM_ERROR_INVALID_BATCH);
    CHECK(sceAjmBatchCancel(context, batch) == ORBIS_AJM_ERROR_INVALID_BATCH);
    error.CheckUnchanged();
}

void CheckStatisticsWorker(u32 context) {
    struct StatisticsOutput {
        AjmSidebandResult result;
        AjmSidebandStatisticsMemory memory;
    } output;
    static_assert(sizeof(output) == 32);
    std::memset(&output, 0xa5, sizeof(output));
    alignas(8) std::array<u8, 128> buffer{};
    constexpr u64 Flags = 7 | (u64{1} << 15);
    auto* end = static_cast<u8*>(sceAjmBatchJobControlBufferRa(
        buffer.data(), AJM_INSTANCE_STATISTICS, Flags, nullptr, 0,
        &output, sizeof(output), nullptr));
    CHECK(end == buffer.data() + 48);
    ErrorSentinel error;
    u32 batch{};
    CHECK(sceAjmBatchStartBuffer(context, buffer.data(), end - buffer.data(),
                                 0, &error.value, &batch) == 0);
    CHECK(sceAjmBatchWait(context, batch, 5000, &error.value) == 0);
    error.CheckUnchanged();
    CHECK(output.result.result == 0 && output.result.internal_result == 0);
    CHECK(output.memory.instance_free == 0x400000);
    CHECK(output.memory.buffer_free == 0x400000);
    CHECK(output.memory.batch_size == 0x4200);
    CHECK(output.memory.input_size == 0x2000);
    CHECK(output.memory.output_size == 0x2000);
    CHECK(output.memory.small_size == 0x400);
}

void CheckRegistryAndInstance(u32 context) {
    CHECK(sceAjmModuleRegister(context, AjmCodecType::Max, 0) == ORBIS_AJM_ERROR_INVALID_PARAMETER);
    // The extracted registry accepts all codec numbers below Max. Constructing
    // unsupported decoders is deliberately outside this registry contract.
    const auto registry_only = static_cast<AjmCodecType>(22);
    CHECK(sceAjmModuleRegister(context, registry_only, 0) == 0);
    CHECK(sceAjmModuleRegister(context, registry_only, 0) == ORBIS_AJM_ERROR_CODEC_ALREADY_REGISTERED);
    CHECK(sceAjmModuleUnregister() == 0);
    CHECK(sceAjmModuleRegister(context, registry_only, 0) == ORBIS_AJM_ERROR_CODEC_ALREADY_REGISTERED);
    u32 instance = 0xdeadbeef;
    CHECK(sceAjmInstanceCreate(context, AjmCodecType::Max, 0, &instance) == ORBIS_AJM_ERROR_INVALID_PARAMETER);
    CHECK(sceAjmInstanceCreate(context, AjmCodecType::At9Dec, 0, &instance) == ORBIS_AJM_ERROR_WRONG_REVISION_FLAG);
    CHECK(sceAjmInstanceCreate(context, AjmCodecType::At9Dec, 1, &instance) == ORBIS_AJM_ERROR_CODEC_NOT_REGISTERED);
    CHECK(instance == 0xdeadbeef);
    CHECK(sceAjmModuleRegister(context, AjmCodecType::At9Dec, 0) == 0);
    CHECK(sceAjmInstanceCreate(context, AjmCodecType::At9Dec, 1, &instance) == 0);
    CHECK(sceAjmInstanceCodecType(instance) == AjmCodecType::At9Dec);
    CHECK((instance & 0x3fff) != 0);
    // Destroy masks the low 14-bit slot; the encoded codec is not validated.
    CHECK(sceAjmInstanceDestroy(context, (instance & 0x3fff) | (22u << 14)) == 0);
    CHECK(sceAjmInstanceDestroy(context, instance) == ORBIS_AJM_ERROR_INVALID_INSTANCE);
}
} // namespace

int main() {
    alarm(15);
    const auto path = std::filesystem::temp_directory_path() /
        ("lumen-ajm-contract-" + std::to_string(getpid()));
    Common::FS::InitializeUserPaths(path);
    EmulatorSettingsImpl::SetInstance(std::make_shared<EmulatorSettingsImpl>());
    Common::Log::Setup("ajm-contract.log");
    u32 context = 0xdeadbeef;
    CHECK(sceAjmInitialize(0, nullptr) == ORBIS_AJM_ERROR_INVALID_PARAMETER);
    CHECK(sceAjmInitialize(1, &context) == ORBIS_AJM_ERROR_INVALID_PARAMETER);
    CHECK(context == 0xdeadbeef);
    CHECK(sceAjmInitialize(0, &context) == 0 && context == 1);
    CHECK(sceAjmModuleRegister(0, AjmCodecType::At9Dec, 1) == ORBIS_AJM_ERROR_INVALID_PARAMETER);
    CHECK(sceAjmModuleRegister(0, AjmCodecType::At9Dec, 0) == ORBIS_AJM_ERROR_INVALID_CONTEXT);
    CHECK(sceAjmBatchWait(0, 1, 0, nullptr) == ORBIS_AJM_ERROR_INVALID_CONTEXT);
    ErrorSentinel error;
    u32 batch = 0xdeadbeef;
    CHECK(sceAjmBatchStartBuffer(context, nullptr, 1, 0, &error.value, &batch) == ORBIS_AJM_ERROR_MALFORMED_BATCH);
    CHECK(batch == 0xdeadbeef);
    error.CheckUnchanged();
    CheckRegistryAndInstance(context);
    CheckEmptyBatch(context, false);
    CheckEmptyBatch(context, true);
    CheckStatisticsWorker(context);
    CHECK(sceAjmFinalize() == 0);
    CHECK(sceAjmFinalize() == 0);
    CheckEmptyBatch(context, false);
    CHECK(sceAjmModuleRegister(context, AjmCodecType::At9Dec, 0) == ORBIS_AJM_ERROR_CODEC_ALREADY_REGISTERED);
    u32 second{};
    CHECK(sceAjmInitialize(0, &second) == 0 && second == 2);
    CheckEmptyBatch(second, false);
    alarm(0);
    std::puts("direct-ajm=PASS initialization/stub-lifecycle/registry/instance-mask/empty-cancel/builder/parser/worker/statistics; codec decoding and process-global teardown excluded");
    std::fflush(nullptr);
    // Finalize is a stub in the reference and owns no shutdown operation. End
    // the fixture process without asserting a global C++ destruction contract.
    std::_Exit(0);
}
