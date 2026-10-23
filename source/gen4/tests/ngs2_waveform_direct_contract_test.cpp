#include "core/libraries/ngs2/ngs2.h"
#include "core/libraries/ngs2/ngs2_error.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace Libraries::Ngs2;

namespace {
int failures;

void Check(bool passed, const char* name) {
    if (!passed) {
        std::fprintf(stderr, "FAIL: %s\n", name);
        ++failures;
    }
}

std::array<u32, 4> Frame(const OrbisNgs2WaveformFormat* format, s32 expected) {
    std::array<u32, 4> result{99, 99, 99, 99};
    const s32 error = sceNgs2GetWaveformFrameInfo(format, &result[0], &result[1], &result[2],
                                                &result[3]);
    Check(error == expected, "frame result");
    return result;
}

bool Empty(const OrbisNgs2WaveformBlock& block) {
    const OrbisNgs2WaveformBlock zero{};
    return std::memcmp(&block, &zero, sizeof(block)) == 0;
}
} // namespace

int main() {
    Check(sceNgs2SystemResetOption(nullptr) == ORBIS_NGS2_ERROR_INVALID_OPTION_ADDRESS,
          "system reset rejects null");
    std::array<u8, 80> option_storage;
    option_storage.fill(0xa5);
    auto* option = reinterpret_cast<OrbisNgs2SystemOption*>(option_storage.data() + 8);
    const OrbisNgs2SystemOption expected_option{64, "", 0, 512, 256, 48000, {0}};
    Check(sceNgs2SystemResetOption(option) == 0 &&
              std::memcmp(option, &expected_option, 64) == 0 &&
              option_storage[7] == 0xa5 && option_storage[72] == 0xa5,
          "system reset writes exactly the extracted64-byte option");
    Check(Frame(nullptr, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_FORMAT) ==
              std::array<u32, 4>{0, 0, 0, 0},
          "null format clears outputs before error");
    OrbisNgs2WaveformFormat format{0x12, 3, 0, 0xffffffff, 99, 99};
    Check(Frame(&format, 0) == std::array<u32, 4>{6, 1, 1, 0},
          "PCM uses channel count and ignores unrelated fields");
    Check(sceNgs2GetWaveformFrameInfo(&format, nullptr, nullptr, nullptr, nullptr) == 0,
          "all optional outputs may be null");
    u32 aliased = 99;
    Check(sceNgs2GetWaveformFrameInfo(&format, &aliased, &aliased, &aliased, &aliased) == 0 &&
              aliased == 0,
          "optional outputs are written in argument order");
    auto alias_format = format;
    Check(sceNgs2GetWaveformFrameInfo(&alias_format, &alias_format.waveformType, nullptr,
                                     nullptr, nullptr) == ORBIS_NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT,
          "output clearing precedes input reads");
    format.waveformType = 0x1c;
    Check(Frame(&format, 0) == std::array<u32, 4>{48, 28, 1, 0}, "0x1c frame geometry");
    format.numChannels = 0;
    Check(Frame(&format, 0) == std::array<u32, 4>{0, 28, 1, 0}, "zero channel geometry");
    format.numChannels = std::numeric_limits<u32>::max();
    Check(Frame(&format, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SIZE) ==
              std::array<u32, 4>{0, 0, 0, 0},
          "frame byte multiplication overflow");
    format = {0x40, 99, 0, 0xfe100000u | (99u << 5) | (3u << 3) | 7u, 99, 99};
    Check(Frame(&format, 0) == std::array<u32, 4>{800, 512, 8, 64},
          "ATRAC9 geometry follows configuration bits without decoder validation");
    format.configData = 0xfe000000;
    Check(Frame(&format, ORBIS_NGS2_ERROR_INVALID_WAVEFORM_CONFIG) ==
              std::array<u32, 4>{0, 0, 0, 0},
          "unsupported ATRAC9 sample index");
    format.waveformType = 0x41;
    Check(Frame(&format, ORBIS_NGS2_ERROR_UNKNOWN_WAVEFORM_FORMAT) ==
              std::array<u32, 4>{0, 0, 0, 0},
          "unknown waveform error");

    OrbisNgs2WaveformBlock block;
    std::memset(&block, 0xff, sizeof(block));
    Check(sceNgs2CalcWaveformBlock(nullptr, 0, 0, nullptr) ==
              ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS,
          "block output checked first");
    Check(sceNgs2CalcWaveformBlock(nullptr, 0, 0, &block) ==
                  ORBIS_NGS2_ERROR_INVALID_WAVEFORM_FORMAT && Empty(block),
          "block output cleared before format validation");
    format = {0x12, 2, 0, 0, 0, 0};
    Check(sceNgs2CalcWaveformBlock(&format, 3, 5, &block) == 0 && block.dataOffset == 12 &&
              block.dataSize == 20 && block.numSamples == 5 && block.numSkipSamples == 0,
          "PCM exact position");
    Check(sceNgs2CalcWaveformBlock(&format, 0x3fffffff, 1, &block) ==
                  ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SIZE && Empty(block),
          "combined end offset overflow");
    format = {0x1c, 2, 0, 0, 0, 0};
    Check(sceNgs2CalcWaveformBlock(&format, 27, 2, &block) == 0 && block.dataOffset == 0 &&
              block.dataSize == 64 && block.numSamples == 56 && block.numSkipSamples == 0,
          "0x1c rounds sample count to complete frames");
    format = {0x40, 0, 0, 0xfe100000u | (99u << 5) | (2u << 3), 0, 0};
    Check(sceNgs2CalcWaveformBlock(&format, 300, 200, &block) == 0 &&
              block.dataOffset == 400 && block.dataSize == 800 && block.numSamples == 200 &&
              block.numSkipSamples == 108 && block.numRepeats == 0 && block.reserved == 0 &&
              block.userData == 0,
          "ATRAC9 frame range and delay");
    Check(sceNgs2CalcWaveformBlock(&format, 0, 0, &block) == 0 && block.dataSize == 400 &&
              block.numSamples == 0 && block.numSkipSamples == 64,
          "ATRAC9 zero requested samples retains delay frame");
    return failures == 0 ? 0 : 1;
}
