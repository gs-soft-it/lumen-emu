#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <string_view>

// Original adapter-side summary of the admitted FEX validator's diagnostic
// interface. Never retain or forward IR, instruction text, IDs or addresses.
namespace Lumen::FexCore::StructuralDiagnostics {

constexpr std::size_t kScanLimit = 4 * 1024 * 1024;
constexpr unsigned kLineLimit = 64;
constexpr std::size_t kOutputSize = 768;

struct StageSummary {
    unsigned stage = 0;
    std::array<unsigned, 10> counts {};
};

inline bool ParseStage(const char* message, StageSummary& output) noexcept {
    if (message == nullptr) return false;
    std::size_t size = 0;
    while (size < 512 && message[size] != '\0') ++size;
    if (size == 512) return false;
    std::string_view input(message, size);
    constexpr std::string_view prefix = "LUMEN_FEX_IR_STRUCTURE_V1 ";
    if (!input.starts_with(prefix)) return false;
    input.remove_prefix(prefix.size());
    constexpr std::array<std::string_view, 11> keys {
        "stage=", "blocks=", "invalid=", "end_missing=", "earlier_exit=",
        "trailing=", "empty=", "metadata=", "state_memory=", "other=", "truncated="};
    StageSummary parsed;
    for (std::size_t index = 0; index < keys.size(); ++index) {
        if (!input.starts_with(keys[index])) return false;
        input.remove_prefix(keys[index].size());
        unsigned value = 0;
        const auto result = std::from_chars(input.data(), input.data() + input.size(), value);
        if (result.ec != std::errc {} || result.ptr == input.data()) return false;
        input.remove_prefix(static_cast<std::size_t>(result.ptr - input.data()));
        if (index == 0) parsed.stage = value;
        else parsed.counts[index - 1] = value;
        if (index + 1 != keys.size()) {
            if (input.empty() || input.front() != ' ') return false;
            input.remove_prefix(1);
        }
    }
    if (!input.empty() || parsed.stage > 4 || parsed.counts[0] > 4096 ||
        parsed.counts[9] > 1) return false;
    for (std::size_t index = 1; index < 9; ++index) {
        if (parsed.counts[index] > parsed.counts[0]) return false;
    }
    if (parsed.counts[2] + parsed.counts[5] + parsed.counts[6] + parsed.counts[7] +
            parsed.counts[8] != parsed.counts[1] ||
        parsed.counts[3] > parsed.counts[1] || parsed.counts[4] > parsed.counts[3]) return false;
    if (parsed.counts[1] == 0 && parsed.counts[9] == 0) return false;
    output = parsed;
    return true;
}

inline void FormatStage(const StageSummary& summary,
                        char (&output)[kOutputSize]) noexcept {
    constexpr std::array<const char*, 5> stages {
        "frontend", "denormal", "x87", "dead_flags", "register_allocation"};
    const char* stage = summary.stage < stages.size() ? stages[summary.stage] : "unknown";
    std::snprintf(output, sizeof(output),
        "LUMEN_FEX_STRUCTURAL_STAGE_V1 stage=%s blocks=%u invalid=%u "
        "end_missing=%u earlier_exit=%u trailing=%u empty=%u metadata=%u "
        "state_memory=%u other=%u truncated=%u", stage,
        summary.counts[0], summary.counts[1], summary.counts[2], summary.counts[3],
        summary.counts[4], summary.counts[5], summary.counts[6], summary.counts[7],
        summary.counts[8], summary.counts[9]);
}

inline bool ExactOne(const char* value) noexcept {
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

struct Summary {
    bool errorsSection = false;
    bool scanTruncated = false;
    bool linesTruncated = false;
    unsigned unknown = 0;
    std::array<unsigned, 11> reasons {};
};

inline Summary Summarize(const char* message) noexcept {
    Summary result;
    if (message == nullptr) {
        return result;
    }
    std::size_t size = 0;
    while (size < kScanLimit && message[size] != '\0') {
        ++size;
    }
    result.scanTruncated = size == kScanLimit;
    const std::string_view text(message, size);
    constexpr std::string_view marker = "Errors:\n";
    const std::size_t markerPosition = text.rfind(marker);
    if (markerPosition == std::string_view::npos ||
        (markerPosition != 0 && text[markerPosition - 1] != '\n')) {
        return result;
    }
    result.errorsSection = true;
    auto errors = text.substr(markerPosition + marker.size());
    constexpr std::array<std::string_view, 11> patterns {
        ": Had destination but with no size",
        ": Had destination but with no register class assigned",
        ": Had destination but with no register assigned",
        "] references invalid %",
        ": True Target Jumps to Op that isn't the begining of a block",
        ": False Target Jumps to Op that isn't the begining of a block",
        ": Jump to Op that isn't the begining of a block",
        " successors which is too many",
        " Failed to end block with EndBlock",
        " Didn't have a block exit IR op as its last instruction",
        " Uses, but reports ",
    };
    unsigned lines = 0;
    while (!errors.empty()) {
        const auto newline = errors.find('\n');
        if (newline == std::string_view::npos && result.scanTruncated) {
            result.linesTruncated = true;
            break;
        }
        const auto line = errors.substr(0, newline);
        errors = newline == std::string_view::npos
            ? std::string_view {} : errors.substr(newline + 1);
        if (line == "Warnings:") {
            break;
        }
        if (line.empty()) {
            continue;
        }
        if (lines == kLineLimit) {
            result.linesTruncated = true;
            break;
        }
        ++lines;
        bool known = false;
        for (std::size_t index = 0; index < patterns.size(); ++index) {
            if (line.find(patterns[index]) != std::string_view::npos) {
                ++result.reasons[index];
                known = true;
                break;
            }
        }
        if (!known) {
            ++result.unknown;
        }
    }
    return result;
}

enum class Kind { Validation, Assert, Throw };

inline void Format(Kind kind, const Summary& summary,
                   char (&output)[kOutputSize]) noexcept {
    const char* kindName = kind == Kind::Validation ? "validation"
        : kind == Kind::Assert ? "assert" : "throw";
    std::snprintf(output, sizeof(output),
        "LUMEN_FEX_STRUCTURAL_ASSERT_V1 kind=%s errors_section=%u "
        "scan_truncated=%u lines_truncated=%u unknown=%u "
        "dest_size=%u dest_class=%u dest_register=%u invalid_reference=%u "
        "true_target=%u false_target=%u jump_target=%u successors=%u "
        "end_block=%u block_exit=%u use_count=%u",
        kindName, static_cast<unsigned>(summary.errorsSection),
        static_cast<unsigned>(summary.scanTruncated),
        static_cast<unsigned>(summary.linesTruncated), summary.unknown,
        summary.reasons[0], summary.reasons[1], summary.reasons[2],
        summary.reasons[3], summary.reasons[4], summary.reasons[5],
        summary.reasons[6], summary.reasons[7], summary.reasons[8],
        summary.reasons[9], summary.reasons[10]);
}

} // namespace Lumen::FexCore::StructuralDiagnostics
