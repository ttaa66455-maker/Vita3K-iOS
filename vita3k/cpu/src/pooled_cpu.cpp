// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <cpu/impl/pooled_cpu.h>

#if defined(VITA3K_PLATFORM_IOS)
#include <cpu/impl/dynarmic_cpu.h>
#include <cpu/jit_invalidation.h>
#include <cpu/slot_pool.h>
#include <cpu/state.h>
#include <util/log.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {
std::size_t configured_threads = 2; // Overridden by the automatic device budget at startup.

struct Worker {
    std::mutex mutex;
    std::unique_ptr<DynarmicCPU> cpu;
    bool optimized = true;
    bool log_code = false;
    bool log_mem = false;
    cpu::JitInvalidation invalidations;
};

struct JitPool {
    MemState *memory;
    cpu::SlotPool admission;
    std::vector<std::unique_ptr<Worker>> workers;

    explicit JitPool(MemState *memory)
        : memory(memory)
        , admission(configured_threads) {
        for (std::size_t i = 0; i < configured_threads; ++i)
            workers.push_back(std::make_unique<Worker>());
        LOG_INFO("iOS shared JIT pool: max_threads={} cache={} MiB per slot (allocated on demand)",
            workers.size(), get_ios_jit_cache_size() / (1024 * 1024));
    }

    void invalidate(Address start, size_t length) {
        if (!length)
            return;
        for (auto &worker : workers) {
            const std::lock_guard lock(worker->mutex);
            worker->invalidations.add(start, length);
            if (worker->cpu)
                worker->cpu->stop();
        }
    }
};

std::shared_ptr<JitPool> get_pool(MemState *memory) {
    static std::mutex mutex;
    static std::weak_ptr<JitPool> existing;
    const std::lock_guard lock(mutex);
    auto pool = existing.lock();
    if (!pool) {
        pool = std::make_shared<JitPool>(memory);
        existing = pool;
    } else if (pool->memory != memory) {
        throw std::runtime_error("Cannot share a JIT pool across guest address spaces");
    }
    return pool;
}

// Guest threads retain registers/TLS while a fixed number of cache-owning
// workers execute bounded slices. Syscalls run AFTER the lease is released,
// so a guest wait/join cannot deadlock a one-slot configuration.
class PooledCPU final : public CPUInterface {
    CPUState *parent;
    std::size_t guest_core_id;
    bool optimized;
    std::shared_ptr<JitPool> pool;
    std::mutex state_mutex;
    CPUContext context;
    std::array<uint32_t, 3> cp15{};
    DynarmicCPU *active = nullptr;
    // Read only from a fault handler on this guest's executing host thread.
    // The worker lease outlives the published pointer, including unwinding.
    std::atomic<DynarmicCPU *> signal_active{ nullptr };
    static_assert(std::atomic<DynarmicCPU *>::is_always_lock_free);
    std::atomic_bool stopped{ false };
    // Reuse warm translations when possible, but never wait for a preferred
    // worker while another slot is free. Only this guest's run thread uses it.
    std::optional<std::size_t> preferred_slot;
    bool breakpoint = false;
    bool log_code = false;
    bool log_mem = false;

    int execute(bool single_step) {
        parent->svc_called = false;
        if (stopped.exchange(false))
            return 0;
        const auto index = pool->admission.acquire(stopped, preferred_slot);
        if (!index) {
            stopped.store(false);
            return 0;
        }
        struct Lease {
            cpu::SlotPool &slots;
            std::size_t index;
            ~Lease() { slots.release(index); }
        } lease{ pool->admission, *index };
        preferred_slot = *index;
        auto &worker = *pool->workers[*index];
        try {
            {
                const std::lock_guard worker_lock(worker.mutex);
                const std::lock_guard state_lock(state_mutex);
                if (stopped.exchange(false))
                    return 0;
                if (!worker.cpu || worker.optimized != optimized
                    || worker.log_code != log_code || worker.log_mem != log_mem) {
                    // Return the old executable region before acquiring another.
                    worker.cpu.reset();
                    worker.cpu = std::make_unique<DynarmicCPU>(parent, *index, optimized, true);
                    worker.optimized = optimized;
                    worker.log_code = log_code;
                    worker.log_mem = log_mem;
                    worker.cpu->set_log_code(log_code);
                    worker.cpu->set_log_mem(log_mem);
                }
                worker.cpu->rebind(parent);
                if (!worker.cpu->ensure_code_cache())
                    return -1;
                worker.invalidations.drain(
                    [&](uint32_t start, std::size_t length) { worker.cpu->invalidate_jit_cache(start, length); },
                    [&] { worker.cpu->clear_translation_cache(); });
                worker.cpu->load_context(context);
                worker.cpu->load_cp15(cp15);
                worker.cpu->clear_exclusive();
                breakpoint = false;
                active = worker.cpu.get();
            }
            struct SignalScope {
                std::atomic<DynarmicCPU *> &pointer;
                ~SignalScope() { pointer.store(nullptr, std::memory_order_relaxed); }
            } signal_scope{ signal_active };
            signal_active.store(worker.cpu.get(), std::memory_order_relaxed);
            const int result = single_step ? worker.cpu->step() : worker.cpu->run();
            signal_active.store(nullptr, std::memory_order_relaxed);
            {
                const std::lock_guard state_lock(state_mutex);
                context = worker.cpu->save_context();
                cp15 = worker.cpu->save_cp15();
                breakpoint = breakpoint || worker.cpu->hit_breakpoint();
                worker.cpu->clear_exclusive();
                active = nullptr;
            }
            return result;
        } catch (const std::exception &error) {
            const std::lock_guard state_lock(state_mutex);
            active = nullptr;
            LOG_ERROR("iOS shared JIT worker failed for guest thread {}: {}", parent->thread_id, error.what());
            return -1;
        }
    }

public:
    PooledCPU(CPUState *parent, std::size_t core_id, bool optimized)
        : parent(parent)
        , guest_core_id(core_id)
        , optimized(optimized)
        , pool(get_pool(parent->mem)) {}

    int run() override { return execute(false); }
    int step() override { return execute(true); }
    void stop() override {
        const std::lock_guard lock(state_mutex);
        stopped.store(true);
        if (active)
            active->stop();
        pool->admission.wake();
    }
    void stop_from_signal() override {
        if (auto *executing = signal_active.load(std::memory_order_relaxed))
            executing->stop();
    }
    uint32_t get_reg(uint8_t idx) override {
        const std::lock_guard lock(state_mutex);
        return active ? active->get_reg(idx) : context.cpu_registers[idx];
    }
    void set_reg(uint8_t idx, uint32_t val) override {
        const std::lock_guard lock(state_mutex);
        if (active)
            active->set_reg(idx, val);
        else
            context.cpu_registers[idx] = val;
    }
    uint32_t get_cpsr() override {
        const std::lock_guard lock(state_mutex);
        return active ? active->get_cpsr() : context.cpsr;
    }
    void set_cpsr(uint32_t val) override {
        const std::lock_guard lock(state_mutex);
        if (active)
            active->set_cpsr(val);
        else
            context.cpsr = val;
    }
    uint32_t get_fpscr() override {
        const std::lock_guard lock(state_mutex);
        return active ? active->get_fpscr() : context.fpscr;
    }
    void set_fpscr(uint32_t val) override {
        const std::lock_guard lock(state_mutex);
        if (active)
            active->set_fpscr(val);
        else
            context.fpscr = val;
    }
    uint32_t get_tpidruro() override {
        const std::lock_guard lock(state_mutex);
        return active ? active->get_tpidruro() : cp15[0];
    }
    void set_tpidruro(uint32_t val) override {
        const std::lock_guard lock(state_mutex);
        if (active)
            active->set_tpidruro(val);
        else
            cp15[0] = val;
    }
    float get_float_reg(uint8_t idx) override {
        const std::lock_guard lock(state_mutex);
        return active ? active->get_float_reg(idx) : context.fpu_registers[idx];
    }
    void set_float_reg(uint8_t idx, float val) override {
        const std::lock_guard lock(state_mutex);
        if (active)
            active->set_float_reg(idx, val);
        else
            context.fpu_registers[idx] = val;
    }
    CPUContext save_context() override {
        const std::lock_guard lock(state_mutex);
        return active ? active->save_context() : context;
    }
    void load_context(const CPUContext &value) override {
        const std::lock_guard lock(state_mutex);
        if (active)
            active->load_context(value);
        else
            context = value;
    }
    uint32_t get_sp() override { return get_reg(13); }
    uint32_t get_lr() override { return get_reg(14); }
    uint32_t get_pc() override { return get_reg(15); }
    void set_sp(uint32_t val) override { set_reg(13, val); }
    void set_lr(uint32_t val) override { set_reg(14, val); }
    void set_pc(uint32_t val) override {
        set_cpsr((get_cpsr() & ~0x20U) | ((val & 1) ? 0x20U : 0));
        set_reg(15, val & ((val & 1) ? ~1U : ~3U));
    }
    bool is_thumb_mode() override { return (get_cpsr() & 0x20) != 0; }
    bool hit_breakpoint() override {
        const std::lock_guard lock(state_mutex);
        return breakpoint;
    }
    void trigger_breakpoint() override {
        {
            const std::lock_guard lock(state_mutex);
            breakpoint = true;
        }
        stop();
    }
    void set_log_code(bool value) override {
        const std::lock_guard lock(state_mutex);
        log_code = value;
    }
    void set_log_mem(bool value) override {
        const std::lock_guard lock(state_mutex);
        log_mem = value;
    }
    bool get_log_code() override {
        const std::lock_guard lock(state_mutex);
        return log_code;
    }
    bool get_log_mem() override {
        const std::lock_guard lock(state_mutex);
        return log_mem;
    }
    void invalidate_jit_cache(Address start, size_t length) override { pool->invalidate(start, length); }
    void clear_exclusive() override {} // Cleared on each worker context switch.
    std::size_t processor_id() const override { return guest_core_id; }
};
} // namespace

void set_ios_jit_threads(std::size_t count) { configured_threads = std::clamp<std::size_t>(count, 1, 64); }
std::size_t get_ios_jit_threads() { return configured_threads; }
CPUInterfacePtr make_ios_pooled_cpu(CPUState *state, std::size_t processor_id, bool cpu_opt) {
    return std::make_unique<PooledCPU>(state, processor_id, cpu_opt);
}
#endif
