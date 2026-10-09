// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace cpu {

// Protected by the worker mutex. Drain only while that worker is not executing.
// Bound pending work even when module loads fan out through every guest thread.
class JitInvalidation {
    struct Range {
        uint64_t begin, end;
    };
    std::array<Range, 32> ranges{};
    std::size_t count = 0;
    bool full = false;

public:
    void add(uint32_t start, std::size_t length) {
        if (!length || full)
            return;
        constexpr uint64_t address_space = uint64_t{ 1 } << 32;
        if (length >= address_space - start) {
            // Avoid wraparound in downstream 32-bit range arithmetic.
            full = true;
            count = 0;
            return;
        }
        Range pending{ start, start + uint64_t(length) };
        for (std::size_t i = 0; i < count;) {
            if (pending.begin <= ranges[i].end && ranges[i].begin <= pending.end) {
                pending.begin = std::min(pending.begin, ranges[i].begin);
                pending.end = std::max(pending.end, ranges[i].end);
                ranges[i] = ranges[--count];
                // A merged interval may now overlap an earlier interval.
                i = 0;
            } else {
                ++i;
            }
        }
        if (count == ranges.size()) {
            full = true;
            count = 0;
        } else {
            ranges[count++] = pending;
        }
    }

    template <typename InvalidateRange, typename ClearAll>
    void drain(InvalidateRange invalidate_range, ClearAll clear_all) {
        if (full) {
            clear_all();
        } else {
            for (std::size_t i = 0; i < count; ++i)
                invalidate_range(static_cast<uint32_t>(ranges[i].begin),
                    static_cast<std::size_t>(ranges[i].end - ranges[i].begin));
        }
        full = false;
        count = 0;
    }
};

} // namespace cpu
