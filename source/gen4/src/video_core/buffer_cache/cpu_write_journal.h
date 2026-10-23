// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <vector>
#include "common/types.h"

namespace VideoCore {

class CpuWriteJournal {
public:
    struct Range {
        VAddr address;
        u64 size;
        bool operator==(const Range&) const = default;
    };

    static CpuWriteJournal& Instance() {
        static CpuWriteJournal journal;
        return journal;
    }

    void Record(VAddr address, u64 size) {
        std::scoped_lock lock{mutex};
        if (overflow) {
            return;
        }
        if (ranges.size() == 4096) {
            overflow = true;
            ranges.clear();
            return;
        }
        ranges.push_back({address, size});
    }

    bool Consume(std::vector<Range>& output) {
        std::scoped_lock lock{mutex};
        output.clear();
        output.swap(ranges);
        const bool result = overflow;
        overflow = false;
        return result;
    }

private:
    std::mutex mutex;
    std::vector<Range> ranges;
    bool overflow{};
};

} // namespace VideoCore
