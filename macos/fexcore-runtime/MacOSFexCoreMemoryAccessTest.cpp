#include "MacOSFexCoreMemoryAccess.h"

#include <cstdint>
#include <initializer_list>
#include <cstdio>

namespace {
struct Case {
    std::uint32_t instruction;
    bool valid;
    std::uint32_t base, width;
    bool write;
};
// Encoded by Apple Clang 21 from original assembly, not a decoder-generated
// oracle. All supported orderings, sizes and RMW operations appear explicitly.
constexpr Case Cases[] {
    {0x88dffca3U, true, 5, 4, false}, // ldar w3,[x5]
    {0xc8dffca3U, true, 5, 8, false}, // ldar x3,[x5]
    {0x08dffca3U, true, 5, 1, false}, // ldarb w3,[x5]
    {0x48dffca3U, true, 5, 2, false}, // ldarh w3,[x5]
    {0x889ffca3U, true, 5, 4, true}, // stlr w3,[x5]
    {0xc89ffca3U, true, 5, 8, true}, // stlr x3,[x5]
    {0x089ffca3U, true, 5, 1, true}, // stlrb w3,[x5]
    {0x489ffca3U, true, 5, 2, true}, // stlrh w3,[x5]
    {0x885f7ca3U, true, 5, 4, false}, // ldxr w3,[x5]
    {0xc85f7ca3U, true, 5, 8, false}, // ldxr x3,[x5]
    {0x885ffca3U, true, 5, 4, false}, // ldaxr w3,[x5]
    {0xc85ffca3U, true, 5, 8, false}, // ldaxr x3,[x5]
    {0x085f7ca3U, true, 5, 1, false}, // ldxrb w3,[x5]
    {0x085ffca3U, true, 5, 1, false}, // ldaxrb w3,[x5]
    {0x485f7ca3U, true, 5, 2, false}, // ldxrh w3,[x5]
    {0x485ffca3U, true, 5, 2, false}, // ldaxrh w3,[x5]
    {0x88077ca3U, true, 5, 4, true}, // stxr w7,w3,[x5]
    {0xc8077ca3U, true, 5, 8, true}, // stxr w7,x3,[x5]
    {0x8807fca3U, true, 5, 4, true}, // stlxr w7,w3,[x5]
    {0xc807fca3U, true, 5, 8, true}, // stlxr w7,x3,[x5]
    {0x08077ca3U, true, 5, 1, true}, // stxrb w7,w3,[x5]
    {0x0807fca3U, true, 5, 1, true}, // stlxrb w7,w3,[x5]
    {0x48077ca3U, true, 5, 2, true}, // stxrh w7,w3,[x5]
    {0x4807fca3U, true, 5, 2, true}, // stlxrh w7,w3,[x5]
    {0x887f10a3U, true, 5, 8, false}, // ldxp w3,w4,[x5]
    {0xc87f10a3U, true, 5, 16, false}, // ldxp x3,x4,[x5]
    {0x887f90a3U, true, 5, 8, false}, // ldaxp w3,w4,[x5]
    {0xc87f90a3U, true, 5, 16, false}, // ldaxp x3,x4,[x5]
    {0x882710a3U, true, 5, 8, true}, // stxp w7,w3,w4,[x5]
    {0xc82710a3U, true, 5, 16, true}, // stxp w7,x3,x4,[x5]
    {0x882790a3U, true, 5, 8, true}, // stlxp w7,w3,w4,[x5]
    {0xc82790a3U, true, 5, 16, true}, // stlxp w7,x3,x4,[x5]
    {0x88a37ca4U, true, 5, 4, true}, // cas w3,w4,[x5]
    {0xc8a37ca4U, true, 5, 8, true}, // cas x3,x4,[x5]
    {0x88e37ca4U, true, 5, 4, true}, // casa w3,w4,[x5]
    {0xc8e37ca4U, true, 5, 8, true}, // casa x3,x4,[x5]
    {0x88a3fca4U, true, 5, 4, true}, // casl w3,w4,[x5]
    {0xc8a3fca4U, true, 5, 8, true}, // casl x3,x4,[x5]
    {0x88e3fca4U, true, 5, 4, true}, // casal w3,w4,[x5]
    {0xc8e3fca4U, true, 5, 8, true}, // casal x3,x4,[x5]
    {0x08a37ca4U, true, 5, 1, true}, // casb w3,w4,[x5]
    {0x08e37ca4U, true, 5, 1, true}, // casab w3,w4,[x5]
    {0x08a3fca4U, true, 5, 1, true}, // caslb w3,w4,[x5]
    {0x08e3fca4U, true, 5, 1, true}, // casalb w3,w4,[x5]
    {0x48a37ca4U, true, 5, 2, true}, // cash w3,w4,[x5]
    {0x48e37ca4U, true, 5, 2, true}, // casah w3,w4,[x5]
    {0x48a3fca4U, true, 5, 2, true}, // caslh w3,w4,[x5]
    {0x48e3fca4U, true, 5, 2, true}, // casalh w3,w4,[x5]
    {0x08227cc4U, true, 6, 8, true}, // casp w2,w3,w4,w5,[x6]
    {0x48227cc4U, true, 6, 16, true}, // casp x2,x3,x4,x5,[x6]
    {0x08627cc4U, true, 6, 8, true}, // caspa w2,w3,w4,w5,[x6]
    {0x48627cc4U, true, 6, 16, true}, // caspa x2,x3,x4,x5,[x6]
    {0x0822fcc4U, true, 6, 8, true}, // caspl w2,w3,w4,w5,[x6]
    {0x4822fcc4U, true, 6, 16, true}, // caspl x2,x3,x4,x5,[x6]
    {0x0862fcc4U, true, 6, 8, true}, // caspal w2,w3,w4,w5,[x6]
    {0x4862fcc4U, true, 6, 16, true}, // caspal x2,x3,x4,x5,[x6]
    {0xb82300a4U, true, 5, 4, true}, // ldadd w3,w4,[x5]
    {0xf82300a4U, true, 5, 8, true}, // ldadd x3,x4,[x5]
    {0x382300a4U, true, 5, 1, true}, // ldaddb w3,w4,[x5]
    {0x782300a4U, true, 5, 2, true}, // ldaddh w3,w4,[x5]
    {0xb8a300a4U, true, 5, 4, true}, // ldadda w3,w4,[x5]
    {0xf8a300a4U, true, 5, 8, true}, // ldadda x3,x4,[x5]
    {0x38a300a4U, true, 5, 1, true}, // ldaddab w3,w4,[x5]
    {0x78a300a4U, true, 5, 2, true}, // ldaddah w3,w4,[x5]
    {0xb86300a4U, true, 5, 4, true}, // ldaddl w3,w4,[x5]
    {0xf86300a4U, true, 5, 8, true}, // ldaddl x3,x4,[x5]
    {0x386300a4U, true, 5, 1, true}, // ldaddlb w3,w4,[x5]
    {0x786300a4U, true, 5, 2, true}, // ldaddlh w3,w4,[x5]
    {0xb8e300a4U, true, 5, 4, true}, // ldaddal w3,w4,[x5]
    {0xf8e300a4U, true, 5, 8, true}, // ldaddal x3,x4,[x5]
    {0x38e300a4U, true, 5, 1, true}, // ldaddalb w3,w4,[x5]
    {0x78e300a4U, true, 5, 2, true}, // ldaddalh w3,w4,[x5]
    {0xb82310a4U, true, 5, 4, true}, // ldclr w3,w4,[x5]
    {0xf82310a4U, true, 5, 8, true}, // ldclr x3,x4,[x5]
    {0x382310a4U, true, 5, 1, true}, // ldclrb w3,w4,[x5]
    {0x782310a4U, true, 5, 2, true}, // ldclrh w3,w4,[x5]
    {0xb8a310a4U, true, 5, 4, true}, // ldclra w3,w4,[x5]
    {0xf8a310a4U, true, 5, 8, true}, // ldclra x3,x4,[x5]
    {0x38a310a4U, true, 5, 1, true}, // ldclrab w3,w4,[x5]
    {0x78a310a4U, true, 5, 2, true}, // ldclrah w3,w4,[x5]
    {0xb86310a4U, true, 5, 4, true}, // ldclrl w3,w4,[x5]
    {0xf86310a4U, true, 5, 8, true}, // ldclrl x3,x4,[x5]
    {0x386310a4U, true, 5, 1, true}, // ldclrlb w3,w4,[x5]
    {0x786310a4U, true, 5, 2, true}, // ldclrlh w3,w4,[x5]
    {0xb8e310a4U, true, 5, 4, true}, // ldclral w3,w4,[x5]
    {0xf8e310a4U, true, 5, 8, true}, // ldclral x3,x4,[x5]
    {0x38e310a4U, true, 5, 1, true}, // ldclralb w3,w4,[x5]
    {0x78e310a4U, true, 5, 2, true}, // ldclralh w3,w4,[x5]
    {0xb82320a4U, true, 5, 4, true}, // ldeor w3,w4,[x5]
    {0xf82320a4U, true, 5, 8, true}, // ldeor x3,x4,[x5]
    {0x382320a4U, true, 5, 1, true}, // ldeorb w3,w4,[x5]
    {0x782320a4U, true, 5, 2, true}, // ldeorh w3,w4,[x5]
    {0xb8a320a4U, true, 5, 4, true}, // ldeora w3,w4,[x5]
    {0xf8a320a4U, true, 5, 8, true}, // ldeora x3,x4,[x5]
    {0x38a320a4U, true, 5, 1, true}, // ldeorab w3,w4,[x5]
    {0x78a320a4U, true, 5, 2, true}, // ldeorah w3,w4,[x5]
    {0xb86320a4U, true, 5, 4, true}, // ldeorl w3,w4,[x5]
    {0xf86320a4U, true, 5, 8, true}, // ldeorl x3,x4,[x5]
    {0x386320a4U, true, 5, 1, true}, // ldeorlb w3,w4,[x5]
    {0x786320a4U, true, 5, 2, true}, // ldeorlh w3,w4,[x5]
    {0xb8e320a4U, true, 5, 4, true}, // ldeoral w3,w4,[x5]
    {0xf8e320a4U, true, 5, 8, true}, // ldeoral x3,x4,[x5]
    {0x38e320a4U, true, 5, 1, true}, // ldeoralb w3,w4,[x5]
    {0x78e320a4U, true, 5, 2, true}, // ldeoralh w3,w4,[x5]
    {0xb82330a4U, true, 5, 4, true}, // ldset w3,w4,[x5]
    {0xf82330a4U, true, 5, 8, true}, // ldset x3,x4,[x5]
    {0x382330a4U, true, 5, 1, true}, // ldsetb w3,w4,[x5]
    {0x782330a4U, true, 5, 2, true}, // ldseth w3,w4,[x5]
    {0xb8a330a4U, true, 5, 4, true}, // ldseta w3,w4,[x5]
    {0xf8a330a4U, true, 5, 8, true}, // ldseta x3,x4,[x5]
    {0x38a330a4U, true, 5, 1, true}, // ldsetab w3,w4,[x5]
    {0x78a330a4U, true, 5, 2, true}, // ldsetah w3,w4,[x5]
    {0xb86330a4U, true, 5, 4, true}, // ldsetl w3,w4,[x5]
    {0xf86330a4U, true, 5, 8, true}, // ldsetl x3,x4,[x5]
    {0x386330a4U, true, 5, 1, true}, // ldsetlb w3,w4,[x5]
    {0x786330a4U, true, 5, 2, true}, // ldsetlh w3,w4,[x5]
    {0xb8e330a4U, true, 5, 4, true}, // ldsetal w3,w4,[x5]
    {0xf8e330a4U, true, 5, 8, true}, // ldsetal x3,x4,[x5]
    {0x38e330a4U, true, 5, 1, true}, // ldsetalb w3,w4,[x5]
    {0x78e330a4U, true, 5, 2, true}, // ldsetalh w3,w4,[x5]
    {0xb82340a4U, true, 5, 4, true}, // ldsmax w3,w4,[x5]
    {0xf82340a4U, true, 5, 8, true}, // ldsmax x3,x4,[x5]
    {0x382340a4U, true, 5, 1, true}, // ldsmaxb w3,w4,[x5]
    {0x782340a4U, true, 5, 2, true}, // ldsmaxh w3,w4,[x5]
    {0xb8a340a4U, true, 5, 4, true}, // ldsmaxa w3,w4,[x5]
    {0xf8a340a4U, true, 5, 8, true}, // ldsmaxa x3,x4,[x5]
    {0x38a340a4U, true, 5, 1, true}, // ldsmaxab w3,w4,[x5]
    {0x78a340a4U, true, 5, 2, true}, // ldsmaxah w3,w4,[x5]
    {0xb86340a4U, true, 5, 4, true}, // ldsmaxl w3,w4,[x5]
    {0xf86340a4U, true, 5, 8, true}, // ldsmaxl x3,x4,[x5]
    {0x386340a4U, true, 5, 1, true}, // ldsmaxlb w3,w4,[x5]
    {0x786340a4U, true, 5, 2, true}, // ldsmaxlh w3,w4,[x5]
    {0xb8e340a4U, true, 5, 4, true}, // ldsmaxal w3,w4,[x5]
    {0xf8e340a4U, true, 5, 8, true}, // ldsmaxal x3,x4,[x5]
    {0x38e340a4U, true, 5, 1, true}, // ldsmaxalb w3,w4,[x5]
    {0x78e340a4U, true, 5, 2, true}, // ldsmaxalh w3,w4,[x5]
    {0xb82350a4U, true, 5, 4, true}, // ldsmin w3,w4,[x5]
    {0xf82350a4U, true, 5, 8, true}, // ldsmin x3,x4,[x5]
    {0x382350a4U, true, 5, 1, true}, // ldsminb w3,w4,[x5]
    {0x782350a4U, true, 5, 2, true}, // ldsminh w3,w4,[x5]
    {0xb8a350a4U, true, 5, 4, true}, // ldsmina w3,w4,[x5]
    {0xf8a350a4U, true, 5, 8, true}, // ldsmina x3,x4,[x5]
    {0x38a350a4U, true, 5, 1, true}, // ldsminab w3,w4,[x5]
    {0x78a350a4U, true, 5, 2, true}, // ldsminah w3,w4,[x5]
    {0xb86350a4U, true, 5, 4, true}, // ldsminl w3,w4,[x5]
    {0xf86350a4U, true, 5, 8, true}, // ldsminl x3,x4,[x5]
    {0x386350a4U, true, 5, 1, true}, // ldsminlb w3,w4,[x5]
    {0x786350a4U, true, 5, 2, true}, // ldsminlh w3,w4,[x5]
    {0xb8e350a4U, true, 5, 4, true}, // ldsminal w3,w4,[x5]
    {0xf8e350a4U, true, 5, 8, true}, // ldsminal x3,x4,[x5]
    {0x38e350a4U, true, 5, 1, true}, // ldsminalb w3,w4,[x5]
    {0x78e350a4U, true, 5, 2, true}, // ldsminalh w3,w4,[x5]
    {0xb82360a4U, true, 5, 4, true}, // ldumax w3,w4,[x5]
    {0xf82360a4U, true, 5, 8, true}, // ldumax x3,x4,[x5]
    {0x382360a4U, true, 5, 1, true}, // ldumaxb w3,w4,[x5]
    {0x782360a4U, true, 5, 2, true}, // ldumaxh w3,w4,[x5]
    {0xb8a360a4U, true, 5, 4, true}, // ldumaxa w3,w4,[x5]
    {0xf8a360a4U, true, 5, 8, true}, // ldumaxa x3,x4,[x5]
    {0x38a360a4U, true, 5, 1, true}, // ldumaxab w3,w4,[x5]
    {0x78a360a4U, true, 5, 2, true}, // ldumaxah w3,w4,[x5]
    {0xb86360a4U, true, 5, 4, true}, // ldumaxl w3,w4,[x5]
    {0xf86360a4U, true, 5, 8, true}, // ldumaxl x3,x4,[x5]
    {0x386360a4U, true, 5, 1, true}, // ldumaxlb w3,w4,[x5]
    {0x786360a4U, true, 5, 2, true}, // ldumaxlh w3,w4,[x5]
    {0xb8e360a4U, true, 5, 4, true}, // ldumaxal w3,w4,[x5]
    {0xf8e360a4U, true, 5, 8, true}, // ldumaxal x3,x4,[x5]
    {0x38e360a4U, true, 5, 1, true}, // ldumaxalb w3,w4,[x5]
    {0x78e360a4U, true, 5, 2, true}, // ldumaxalh w3,w4,[x5]
    {0xb82370a4U, true, 5, 4, true}, // ldumin w3,w4,[x5]
    {0xf82370a4U, true, 5, 8, true}, // ldumin x3,x4,[x5]
    {0x382370a4U, true, 5, 1, true}, // lduminb w3,w4,[x5]
    {0x782370a4U, true, 5, 2, true}, // lduminh w3,w4,[x5]
    {0xb8a370a4U, true, 5, 4, true}, // ldumina w3,w4,[x5]
    {0xf8a370a4U, true, 5, 8, true}, // ldumina x3,x4,[x5]
    {0x38a370a4U, true, 5, 1, true}, // lduminab w3,w4,[x5]
    {0x78a370a4U, true, 5, 2, true}, // lduminah w3,w4,[x5]
    {0xb86370a4U, true, 5, 4, true}, // lduminl w3,w4,[x5]
    {0xf86370a4U, true, 5, 8, true}, // lduminl x3,x4,[x5]
    {0x386370a4U, true, 5, 1, true}, // lduminlb w3,w4,[x5]
    {0x786370a4U, true, 5, 2, true}, // lduminlh w3,w4,[x5]
    {0xb8e370a4U, true, 5, 4, true}, // lduminal w3,w4,[x5]
    {0xf8e370a4U, true, 5, 8, true}, // lduminal x3,x4,[x5]
    {0x38e370a4U, true, 5, 1, true}, // lduminalb w3,w4,[x5]
    {0x78e370a4U, true, 5, 2, true}, // lduminalh w3,w4,[x5]
    {0xb82380a4U, true, 5, 4, true}, // swp w3,w4,[x5]
    {0xf82380a4U, true, 5, 8, true}, // swp x3,x4,[x5]
    {0x382380a4U, true, 5, 1, true}, // swpb w3,w4,[x5]
    {0x782380a4U, true, 5, 2, true}, // swph w3,w4,[x5]
    {0xb8a380a4U, true, 5, 4, true}, // swpa w3,w4,[x5]
    {0xf8a380a4U, true, 5, 8, true}, // swpa x3,x4,[x5]
    {0x38a380a4U, true, 5, 1, true}, // swpab w3,w4,[x5]
    {0x78a380a4U, true, 5, 2, true}, // swpah w3,w4,[x5]
    {0xb86380a4U, true, 5, 4, true}, // swpl w3,w4,[x5]
    {0xf86380a4U, true, 5, 8, true}, // swpl x3,x4,[x5]
    {0x386380a4U, true, 5, 1, true}, // swplb w3,w4,[x5]
    {0x786380a4U, true, 5, 2, true}, // swplh w3,w4,[x5]
    {0xb8e380a4U, true, 5, 4, true}, // swpal w3,w4,[x5]
    {0xf8e380a4U, true, 5, 8, true}, // swpal x3,x4,[x5]
    {0x38e380a4U, true, 5, 1, true}, // swpalb w3,w4,[x5]
    {0x78e380a4U, true, 5, 2, true}, // swpalh w3,w4,[x5]
    {0xd503201fU, false, 0, 0, false}, // nop
    {0xd65f03c0U, false, 0, 0, false}, // ret
    {0xf94000a3U, false, 0, 0, false}, // ldr x3,[x5]
    {0xf90000a3U, false, 0, 0, false}, // str x3,[x5]
    {0xa94010a3U, false, 0, 0, false}, // ldp x3,x4,[x5]
    {0xa90010a3U, false, 0, 0, false}, // stp x3,x4,[x5]
    {0xc8dfffe3U, true, 31, 8, false}, // ldar x3,[sp]
    {0xc89fffe3U, true, 31, 8, true}, // stlr x3,[sp]
};
}

int main() {
    using namespace Lumen::MacOS::FexCore;
    unsigned checks = 0, failures = 0;
    for (const auto& test : Cases) {
        const auto decoded = DecodeMemoryAccess(test.instruction);
        ++checks;
        if (decoded.valid != test.valid || (test.valid &&
            (decoded.baseRegister != test.base || decoded.byteOffset != 0 ||
             decoded.width != test.width || decoded.write != test.write))) {
            std::fprintf(stderr, "MemoryAccess: FAIL opcode=%08x valid=%u base=%u width=%u write=%u\n",
                         test.instruction, decoded.valid, decoded.baseRegister, decoded.width, decoded.write);
            ++failures;
        }
        if (!test.valid) continue;
        // Register selection must not change the memory footprint. In
        // particular, base 31 is SP and must not become a zero-register access.
        for (unsigned base = 0; base != 32; ++base) {
            const auto changed = DecodeMemoryAccess((test.instruction & ~0x3e0U) | (base << 5));
            ++checks;
            if (!changed.valid || changed.baseRegister != base || changed.byteOffset != 0 ||
                changed.width != test.width || changed.write != test.write) ++failures;
        }
    }
    constexpr std::uint32_t denied[] {
        0U, 0xffffffffU, 0xf8bfc4a3U, // reserved LDAPR encoding
        0x087f10a3U, 0x487f10a3U,   // invalid byte/halfword exclusive pair
        0x08237cc4U, 0x08227cc5U,   // CASP odd pair registers
        0xf82390a4U, 0xf823f0a4U,   // unqualified LSE operation
    };
    for (auto instruction : denied) {
        ++checks;
        if (DecodeMemoryAccess(instruction).valid) ++failures;
    }
    for (auto esr : {0x92000021U, 0x92000061U}) {
        ++checks;
        if (!IsDataAlignmentFault(esr)) ++failures;
    }
    for (auto esr : {0x9200004fU, 0x9200000fU, 0x82000021U, 0x96000021U, 0U}) {
        ++checks;
        if (IsDataAlignmentFault(esr)) ++failures;
    }
    for (unsigned width : {1U, 2U, 4U, 8U, 16U}) {
        for (std::uintptr_t phase = 0; phase != 32; ++phase) {
            ++checks;
            const bool scalar = width == 2 || width == 4 || width == 8;
            if (IsDirectUnalignedScalarAccess(4096 + phase, width) !=
                (scalar && phase % 16 + width > 16)) ++failures;
            std::uintptr_t first {}, length {};
            ++checks;
            if (!ResolveHelperMemoryEnvelope(4096 + phase, width, &first, &length) ||
                first > 4096 + phase || first % 16 != 0 || length % 16 != 0 ||
                first + length < 4096 + phase + width) ++failures;
        }
    }
    for (auto address : {UINTPTR_MAX, UINTPTR_MAX - 4, UINTPTR_MAX - 15}) {
        std::uintptr_t first = 17, length = 19;
        ++checks;
        if (ResolveHelperMemoryEnvelope(address, 8, &first, &length) ||
            first != 17 || length != 19) ++failures;
    }
    constexpr std::uint32_t scalarOracle[] {
        0x48dffc20U, 0x489ffc20U, 0x88dffc20U, 0x889ffc20U,
        0xc8dffc20U, 0xc89ffc20U,
    };
    unsigned oracle {};
    for (auto width : {2U, 4U, 8U}) for (bool write : {false, true}) {
        ++checks;
        const auto instruction = DirectUnalignedScalarInstruction(width, write);
        const auto access = DecodeMemoryAccess(instruction);
        if (instruction != scalarOracle[oracle++] || !access.valid ||
            access.width != width || access.write != write || access.baseRegister != 1) ++failures;
    }
    for (auto width : {0U, 1U, 16U, UINT32_MAX}) {
        ++checks;
        if (DirectUnalignedScalarInstruction(width, true) != 0) ++failures;
    }
    std::printf("LUMEN_MACOS_FEXCORE_MEMORY_ACCESS_TEST_V1 checks=%u failures=%u pass=%u\n",
                checks, failures, failures == 0);
    return failures != 0;
}
