// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>

namespace vita3k_ios {

// Any thread may request reclamation; only the render thread consumes it.
// Keep a deferred request pending: a burst must not become a lost OS warning.
class MemoryReclaimRequests {
public:
    using Clock = std::chrono::steady_clock;

    bool request() {
        return !pending.exchange(true, std::memory_order_acq_rel);
    }

    bool has_pending() const {
        return pending.load(std::memory_order_acquire);
    }

    bool consume(Clock::time_point now, uint64_t available_bytes) {
        if (!has_pending())
            return false;

        // 180 MiB matches the monitor's 50% proactive-pressure threshold.
        // Unknown or low headroom must retain the original immediate response.
        constexpr uint64_t low_headroom = 180ull * 1024 * 1024;
        const bool urgent = available_bytes == 0 || available_bytes <= low_headroom;
        if (!urgent && last_reclaim && now - *last_reclaim < std::chrono::seconds(1))
            return false;

        if (!pending.exchange(false, std::memory_order_acq_rel))
            return false;
        last_reclaim = now;
        return true;
    }

private:
    std::atomic<bool> pending{ false };
    std::optional<Clock::time_point> last_reclaim;
};

} // namespace vita3k_ios
