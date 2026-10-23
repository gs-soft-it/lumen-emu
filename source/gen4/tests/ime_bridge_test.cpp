// SPDX-License-Identifier: GPL-2.0-only
#include "lumen/ime_bridge.h"
#include <cstdio>
#include <stdexcept>

int main() {
    using namespace Lumen::Gen4::Ime;
    auto require = [](bool condition) { if (!condition) throw std::runtime_error("IME ownership contract failed"); };
    HostSnapshot snapshot; snapshot.max_text_length = 8;
    Publish(snapshot);
    const auto generation = State().snapshot.generation;
    require(!HostReplaceText(generation + 1, u"stale", 2));
    require(!HostReplaceText(generation, u"012345678", 0));
    require(!HostReplaceText(generation, u"ok", 3));
    const std::u16string unicode = u"ciao\U0001F600";
    require(HostReplaceText(generation, unicode, unicode.size()));
    require(HostAccept(generation));
    require(!HostReplaceText(generation, u"late", 0));
    const auto command = TakeCommand();
    require(command && command->text == unicode && command->finish == 0);
    require(!HostCancel(generation));
    Close();
    require(!HostReplaceText(generation, u"closed", 0));
    Publish(snapshot);
    const auto replacement = State().snapshot.generation;
    require(replacement > generation && !HostAccept(generation));
    require(HostCancel(replacement));
    require(TakeCommand()->finish == 1);
    Close();
    std::puts("Gen4 IME generation and owned text contract PASS");
}
