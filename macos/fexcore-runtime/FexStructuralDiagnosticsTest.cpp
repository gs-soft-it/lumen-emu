#include "FexStructuralDiagnostics.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace Diagnostics = Lumen::FexCore::StructuralDiagnostics;

int main() {
    unsigned failures = 0;
    auto check = [&failures](bool result, const char* label) {
        if (!result) {
            std::fprintf(stderr, "FAIL: %s\n", label);
            ++failures;
        }
    };
    check(Diagnostics::ExactOne("1"), "exact one");
    for (const char* value : std::array<const char*, 6> {nullptr, "", "0", "01", "true", "1 "}) {
        check(!Diagnostics::ExactOne(value), "selector rejected");
    }

    // Synthetic strings only. A matching phrase outside Errors must not count,
    // and neither private-looking text nor node IDs may reach the output.
    const char* message =
        "IR PRIVATE_DUMMY_0xfeedbeef %998: Arg[0] references invalid %999\n"
        "Errors:\n"
        "%71: Arg[2] references invalid %72\n"
        "%73: Arg[0] references invalid %74\n"
        "%75 Has 9 Uses, but reports 2\n"
        "PRIVATE_ERROR_DUMMY_0x12345678\n\n";
    const auto summary = Diagnostics::Summarize(message);
    check(summary.errorsSection && !summary.scanTruncated && !summary.linesTruncated,
          "bounded Errors section found");
    check(summary.reasons[3] == 2 && summary.reasons[10] == 1 && summary.unknown == 1,
          "structural reason counts");
    char output[Diagnostics::kOutputSize];
    Diagnostics::Format(Diagnostics::Kind::Validation, summary, output);
    check(std::strstr(output, "invalid_reference=2") != nullptr &&
          std::strstr(output, "use_count=1") != nullptr &&
          std::strstr(output, "unknown=1") != nullptr, "static summary fields");
    for (const char* forbidden : {"PRIVATE", "feedbeef", "12345678", "%71", "%998", "Arg["}) {
        check(std::strstr(output, forbidden) == nullptr, "no raw text echoed");
    }
    check(!Diagnostics::Summarize("IR only: references invalid %1\n").errorsSection,
          "no Errors section means no classification");
    check(!Diagnostics::Summarize("PRIVATEErrors:\n%1 Has 9 Uses, but reports 2\n").errorsSection,
          "section marker must begin a line");
    check(!Diagnostics::Summarize(nullptr).errorsSection, "null diagnostic");

    std::string manyLines = "Errors:\n";
    for (unsigned index = 0; index < Diagnostics::kLineLimit + 5; ++index) {
        manyLines += "%1: Had destination but with no size\n";
    }
    const auto capped = Diagnostics::Summarize(manyLines.c_str());
    check(capped.linesTruncated && capped.reasons[0] == Diagnostics::kLineLimit,
          "line and counter cap");
    std::string tooLong(Diagnostics::kScanLimit + 1, 'x');
    const auto scanCapped = Diagnostics::Summarize(tooLong.c_str());
    check(scanCapped.scanTruncated && !scanCapped.errorsSection, "scan cap");
    tooLong.replace(0, 8, "Errors:\n");
    const auto partial = Diagnostics::Summarize(tooLong.c_str());
    check(partial.scanTruncated && partial.linesTruncated && partial.unknown == 0,
          "partial truncated line never classified");

    Diagnostics::Format(Diagnostics::Kind::Assert, {}, output);
    check(std::strstr(output, "kind=assert") != nullptr, "static assertion marker");
    Diagnostics::Format(Diagnostics::Kind::Throw, {}, output);
    check(std::strstr(output, "kind=throw") != nullptr, "static throw marker");

    constexpr const char* stageMessage =
        "LUMEN_FEX_IR_STRUCTURE_V1 stage=4 blocks=8 invalid=1 end_missing=0 earlier_exit=1 "
        "trailing=1 empty=0 metadata=1 state_memory=0 other=0 truncated=0";
    Diagnostics::StageSummary stage;
    check(Diagnostics::ParseStage(stageMessage, stage), "closed stage schema");
    Diagnostics::FormatStage(stage, output);
    check(std::strstr(output, "stage=register_allocation") != nullptr &&
          std::strstr(output, "metadata=1") != nullptr, "static stage formatter");
    const auto originalStage = stage;
    for (const std::string& value : {
             std::string(stageMessage) + " PRIVATE_DUMMY_0xfeedbeef",
             std::string("LUMEN_FEX_IR_STRUCTURE_V1 stage=99999999999999999999999999999"),
             std::string("LUMEN_FEX_IR_STRUCTURE_V1 stage=-1"),
             std::string("LUMEN_FEX_IR_STRUCTURE_V1 stage=0 blocks=9999999999")}) {
        check(!Diagnostics::ParseStage(value.c_str(), stage), "malformed stage schema rejected");
        check(stage.stage == originalStage.stage && stage.counts == originalStage.counts,
              "parser rejection preserves destination");
    }
    std::printf("FEX structural diagnostics: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
