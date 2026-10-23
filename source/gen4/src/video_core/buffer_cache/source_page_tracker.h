// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <unordered_map>
#include "common/types.h"

namespace VideoCore {

class SourcePageTracker {
public:
    struct Write {
        u64 generation{};
        u64 buffer{};
    };

    void Register(VAddr page) {
        if (pages.size() < 256 || pages.contains(page)) {
            pages.try_emplace(page);
        }
    }

    Write Get(VAddr page) const {
        const auto it = pages.find(page);
        return it == pages.end() ? Write{} : it->second;
    }

    u64 Generation() const {
        return generation;
    }

    void Mark(VAddr address, u64 size, u64 buffer) {
        const VAddr end = address + size;
        const VAddr first_page = address & ~VAddr{4095};
        bool advanced = false;
        const auto update = [&](Write& write) {
            if (!advanced) {
                ++generation;
                advanced = true;
            }
            write = {generation, buffer};
        };
        if (pages.size() < (end - first_page) / 4096) {
            for (auto& [page, write] : pages) {
                if (page < end && page + 4096 > address) {
                    update(write);
                }
            }
        } else {
            for (VAddr page = first_page; page < end; page += 4096) {
                if (const auto it = pages.find(page); it != pages.end()) {
                    update(it->second);
                }
            }
        }
    }

private:
    std::unordered_map<VAddr, Write> pages;
    u64 generation{};
};

} // namespace VideoCore
