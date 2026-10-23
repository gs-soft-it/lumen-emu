// SPDX-License-Identifier: MIT
#include "core/guest_cpu/guest_printf.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace Core::GuestCpu;
struct Range { uintptr_t begin; size_t size; };
static void Require(bool value) { if (!value) std::abort(); }
int main() {
    std::vector<Range> ranges;
    auto allow = [&](const void* p, size_t n) { ranges.push_back({reinterpret_cast<uintptr_t>(p), n}); };
    HleCallFrame frame{};
    frame.validate_context = &ranges;
    frame.validate_range = [](void* context, uintptr_t p, size_t n, bool) {
        for (auto range : *static_cast<std::vector<Range>*>(context))
            if (p >= range.begin && p - range.begin <= range.size && n <= range.size - (p - range.begin)) return true;
        return false;
    };
    const char format[] = "%lld %s %.2f %d %u %x %hhd%n";
    const char string[] = "guest";
    int written{};
    u64 stack[]{0, 4, 0xff, 0xfe, reinterpret_cast<u64>(&written)};
    allow(format,sizeof(format)); allow(string,sizeof(string)); allow(stack,sizeof(stack)); allow(&written,sizeof(written));
    frame.rsp=reinterpret_cast<u64>(stack);
    frame.gpr[1]=static_cast<u64>(-42ll); frame.gpr[8]=reinterpret_cast<u64>(string); frame.gpr[9]=3;
    frame.xmm[0][0]=std::bit_cast<u64>(1.25);
    GuestFormatArguments args{frame,3}; auto result=FormatGuestPrintf(args,reinterpret_cast<u64>(format));
    Require(result.error==GuestFormatError::None && result.text=="-42 guest 1.25 3 4 ff -2");
    Require(result.count_writes.size()==1 && result.count_writes[0].value==result.text.size() && written==0);
    const char width[]="%*.*s"; allow(width,sizeof(width));
    frame.gpr[1]=-5;frame.gpr[8]=2;frame.gpr[9]=reinterpret_cast<u64>(string);
    GuestFormatArguments widths{frame,3}; result=FormatGuestPrintf(widths,reinterpret_cast<u64>(width));
    Require(result.error==GuestFormatError::None && result.text=="gu   ");
    const char spilled[]="%d";allow(spilled,sizeof(spilled));frame.rsp=1;
    GuestFormatArguments bad_stack{frame,6}; result=FormatGuestPrintf(bad_stack,reinterpret_cast<u64>(spilled));
    Require(result.error==GuestFormatError::Fault);
    const char positional[]="%2$d";allow(positional,sizeof(positional));
    GuestFormatArguments unsupported{frame,0}; result=FormatGuestPrintf(unsupported,reinterpret_cast<u64>(positional));
    Require(result.error==GuestFormatError::Unsupported);
    alignas(16) u64 saved_regs[22]{};saved_regs[0]=123; saved_regs[6]=std::bit_cast<u64>(3.5);
    allow(saved_regs,sizeof(saved_regs)); const char saved_format[]="%d %.1f";allow(saved_format,sizeof(saved_format));
    GuestFormatArguments saved{frame,GuestVaList{0,48,0,reinterpret_cast<u64>(saved_regs)}};
    result=FormatGuestPrintf(saved,reinterpret_cast<u64>(saved_format));
    Require(result.error==GuestFormatError::None && result.text=="123 3.5");
    std::puts("guest printf ABI: PASS");
}
