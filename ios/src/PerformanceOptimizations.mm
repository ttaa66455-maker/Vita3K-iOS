// iOS performance helpers: memory pressure, thermal state, stability defaults.
//
// Implemented as Objective-C++ so we can read NSProcessInfo.thermalState and
// Mach task info without pulling UIKit into every translation unit.

#include <vita3k_ios/MemoryReclaim.h>
#include <vita3k_ios/PerformanceOptimizations.h>

#include <util/log.h>

#include <algorithm>
#include <atomic>
#include <mach/mach.h>
#include <os/proc.h>

#define Ptr MacTypesPtr
#import <Foundation/Foundation.h>
#undef Ptr

namespace vita3k_ios {
namespace {

MemoryReclaimRequests g_gc_requests;

uint64_t task_rss_bytes() {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    const kern_return_t kr = task_info(mach_task_self(), TASK_VM_INFO,
        reinterpret_cast<task_info_t>(&info), &count);
    if (kr != KERN_SUCCESS)
        return 0;
    return static_cast<uint64_t>(info.phys_footprint);
}

} // namespace

int MemoryMonitor::get_memory_pressure() {
    const uint64_t available = get_available_bytes();
    if (available == 0)
        return 0;

    // A11 / 2–3 GB devices jetsam earlier. Trigger GC and quality scaling
    // while there is still headroom instead of waiting until available < 120 MiB.
    constexpr uint64_t soft_limit = 280ull * 1024 * 1024;
    constexpr uint64_t hard_limit = 80ull * 1024 * 1024;

    if (available >= soft_limit)
        return 0;
    if (available <= hard_limit)
        return 100;

    const double t = static_cast<double>(soft_limit - available)
        / static_cast<double>(soft_limit - hard_limit);
    return static_cast<int>(t * 100.0);
}

uint64_t MemoryMonitor::get_rss_mb() {
    return task_rss_bytes() / (1024 * 1024);
}

uint64_t MemoryMonitor::get_available_bytes() {
#if defined(__APPLE__)
    return os_proc_available_memory();
#else
    return 0;
#endif
}

bool MemoryMonitor::is_memory_critical() {
    return get_memory_pressure() >= 70;
}

void MemoryMonitor::request_gc() {
    if (!g_gc_requests.request())
        return;
    LOG_WARN("iOS MemoryMonitor: GC requested (rss={} MiB, available={} MiB)",
        get_rss_mb(), get_available_bytes() / (1024 * 1024));
}

bool MemoryMonitor::consume_gc_request() {
    if (!g_gc_requests.has_pending())
        return false;
    return g_gc_requests.consume(MemoryReclaimRequests::Clock::now(), get_available_bytes());
}

ThermalThrottleManager::ThermalState ThermalThrottleManager::get_thermal_state() {
    @autoreleasepool {
        const NSProcessInfoThermalState state = NSProcessInfo.processInfo.thermalState;
        switch (state) {
        case NSProcessInfoThermalStateNominal:
            return ThermalState::NOMINAL;
        case NSProcessInfoThermalStateFair:
            return ThermalState::FAIR;
        case NSProcessInfoThermalStateSerious:
            return ThermalState::SERIOUS;
        case NSProcessInfoThermalStateCritical:
            return ThermalState::CRITICAL;
        default:
            return ThermalState::NOMINAL;
        }
    }
}

ThermalThrottleManager::ScalingAdvice ThermalThrottleManager::advice_for(
    ThermalState state, const ScalingAdvice &baseline) {
    ScalingAdvice advice = baseline;
    switch (state) {
    case ThermalState::NOMINAL:
        break;
    case ThermalState::FAIR:
        // Mild heat on A11-class: prefer async, lower aniso, and drop surface
        // sync cost early so staging readbacks do not compound with thermal.
        advice.async_pipeline_compilation = true;
        if (advice.anisotropic_filtering > 1)
            advice.anisotropic_filtering = 1;
        advice.surface_sync = false;
        if (advice.resolution_multiplier > 0.75f)
            advice.resolution_multiplier = 0.75f;
        break;
    case ThermalState::SERIOUS:
        // Noticeable heat: half-res, disable high-accuracy and surface sync.
        advice.resolution_multiplier = 0.5f;
        advice.high_accuracy = false;
        advice.surface_sync = false;
        advice.anisotropic_filtering = 1;
        advice.async_pipeline_compilation = true;
        break;
    case ThermalState::CRITICAL:
        // Protect the device: half-res, cheapest GPU path.
        advice.resolution_multiplier = 0.5f;
        advice.high_accuracy = false;
        advice.surface_sync = false;
        advice.anisotropic_filtering = 1;
        advice.async_pipeline_compilation = true;
        break;
    }
    return advice;
}

const char *ThermalThrottleManager::state_name(ThermalState state) {
    switch (state) {
    case ThermalState::NOMINAL: return "nominal";
    case ThermalState::FAIR: return "fair";
    case ThermalState::SERIOUS: return "serious";
    case ThermalState::CRITICAL: return "critical";
    }
    return "unknown";
}

StabilityDefaults recommended_stability_defaults() {
    StabilityDefaults d;
    d.async_pipeline_compilation = true;
    d.cpu_opt = true;
    d.shader_cache = true;
    d.texture_cache = true;
    // A11-class first-run defaults: half-res keeps headroom for surface
    // readbacks and texture hashing under jetsam pressure.
    d.high_accuracy = true;
    d.anisotropic_filtering = 1;
    d.resolution_multiplier = 0.5f;
    d.v_sync = false;
    d.fps_limit = 30;
    return d;
}

uint64_t recommended_guest_memory_bytes() {
    @autoreleasepool {
        const uint64_t physical = static_cast<uint64_t>(NSProcessInfo.processInfo.physicalMemory);
        // Keep the guest well below Jetsam. A11 (iPhone 8/X, ~2–3 GB) is the
        // primary target: 448 MiB leaves more room for host surface staging
        // and Vulkan pipeline caches than the previous 512 MiB budget.
        if (physical >= 4ull * 1024 * 1024 * 1024)
            return 768ull * 1024 * 1024;
        if (physical >= 3ull * 1024 * 1024 * 1024)
            return 576ull * 1024 * 1024;
        return 448ull * 1024 * 1024;
    }
}

PerfSample sample_runtime_pressure() {
    PerfSample sample;
    sample.thermal = ThermalThrottleManager::get_thermal_state();
    sample.memory_pressure = MemoryMonitor::get_memory_pressure();
    sample.rss_mb = MemoryMonitor::get_rss_mb();
    sample.available_mb = MemoryMonitor::get_available_bytes() / (1024 * 1024);
    return sample;
}

} // namespace vita3k_ios
