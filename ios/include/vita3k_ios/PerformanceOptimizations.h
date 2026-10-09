#pragma once

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>

namespace vita3k_ios {

/**
 * Frame-rate limiter tuned for MoltenVK's variable present times.
 * Uses a monotonic clock and residual nanosleep so late frames do not
 * cascade into multi-frame sleep debt (a common source of micro-stutter).
 */
class FrameRateLimiter {
public:
    explicit FrameRateLimiter(int target_fps = 30)
        : target_fps_(target_fps)
        , frame_budget_ns_(frame_budget_ns(target_fps))
        , next_deadline_(std::chrono::steady_clock::now()) {
    }

    /** Call once per presented frame. Sleeps only the remaining budget. */
    void throttle() {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_deadline_) {
            const auto remaining = next_deadline_ - now;
            // Never busy-spin on iOS. Sleeping for the complete remaining
            // budget lets A11 cores enter a lower-power state instead of
            // burning a core for the last sub-millisecond of every frame.
            std::this_thread::sleep_for(remaining);
        }

        // Advance deadline from the planned slot, not from "now", so a late
        // frame does not push every subsequent frame later as well.
        next_deadline_ += std::chrono::nanoseconds(frame_budget_ns_);
        // If we fell more than 2 frames behind, resync to avoid a long catch-up sleep.
        const auto behind = std::chrono::steady_clock::now() - next_deadline_;
        if (behind > std::chrono::nanoseconds(frame_budget_ns_ * 2)) {
            next_deadline_ = std::chrono::steady_clock::now() + std::chrono::nanoseconds(frame_budget_ns_);
        }
    }

    int64_t get_last_frame_time_us() const {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - (next_deadline_ - std::chrono::nanoseconds(frame_budget_ns_))).count();
    }

    void reset() {
        next_deadline_ = std::chrono::steady_clock::now();
    }

    void set_target_fps(int target_fps) {
        target_fps_ = target_fps;
        frame_budget_ns_ = frame_budget_ns(target_fps);
        reset();
    }

    int target_fps() const { return target_fps_; }

private:
    static int64_t frame_budget_ns(int target_fps) {
        if (target_fps <= 0 || target_fps > 1000000)
            throw std::invalid_argument("target_fps must be between 1 and 1000000");
        return 1'000'000'000LL / target_fps;
    }

    int target_fps_;
    int64_t frame_budget_ns_;
    std::chrono::steady_clock::time_point next_deadline_;
};

/** Memory pressure helpers for iOS jetsam avoidance. */
class MemoryMonitor {
public:
    /** 0–100, where 100 is critical (near jetsam). */
    static int get_memory_pressure();

    /** Resident set size in MiB. */
    static uint64_t get_rss_mb();

    /** Available bytes before jetsam (0 if unknown). */
    static uint64_t get_available_bytes();

    /** True when available memory is under ~15% of a typical app budget. */
    static bool is_memory_critical();

    /** Hint the runtime to drop non-essential caches (shader/texture soft). */
    static void request_gc();

    /** Render-thread only: consume a request, coalescing bursts when headroom permits. */
    static bool consume_gc_request();
};

/**
 * Thermal monitoring. When the device heats up we scale quality down so the
 * OS does not hard-throttle the whole process (which feels like random freezes).
 */
class ThermalThrottleManager {
public:
    enum class ThermalState : int {
        NOMINAL = 0,
        FAIR = 1,
        SERIOUS = 2,
        CRITICAL = 3,
    };

    static ThermalState get_thermal_state();

    struct ScalingAdvice {
        float resolution_multiplier = 1.0f;
        bool high_accuracy = true;
        bool async_pipeline_compilation = true;
        int anisotropic_filtering = 4;
        bool surface_sync = true;
    };

    /** Recommended settings for the given thermal state (starting from baseline). */
    static ScalingAdvice advice_for(ThermalState state, const ScalingAdvice &baseline);

    /** Human-readable name for logs. */
    static const char *state_name(ThermalState state);
};

/**
 * One-shot recommended defaults for stable first-run behaviour on iPhone/iPad.
 * Call when building the initial config before a session starts.
 */
struct StabilityDefaults {
    bool async_pipeline_compilation = true;
    bool cpu_opt = true;
    bool shader_cache = true;
    bool texture_cache = true;
    bool high_accuracy = true;
    int anisotropic_filtering = 4;
    float resolution_multiplier = 1.0f;
    bool v_sync = true;
    int fps_limit = 30;
};

StabilityDefaults recommended_stability_defaults();

/** Periodic sample written by the guest watchdog. */
struct PerfSample {
    ThermalThrottleManager::ThermalState thermal = ThermalThrottleManager::ThermalState::NOMINAL;
    int memory_pressure = 0;
    uint64_t rss_mb = 0;
    uint64_t available_mb = 0;
};

PerfSample sample_runtime_pressure();

/** Guest RAM ceiling derived from physical device memory, not a user setting. */
uint64_t recommended_guest_memory_bytes();

} // namespace vita3k_ios
