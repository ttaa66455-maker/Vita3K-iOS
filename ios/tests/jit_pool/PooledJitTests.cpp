#include <cpu/disasm/functions.h>
#include <cpu/functions.h>
#include <cpu/impl/pooled_cpu.h>
#include <cpu/state.h>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <mem/functions.h>
#include <mem/ptr.h>
#include <thread>
#include <vector>
// Only diagnostics are adapted. All guest execution, JIT and allocation code
// below is the production source, compiled with VITA3K_PLATFORM_IOS.
bool init(DisasmState &) { return true; }
std::string disassemble(DisasmState &, const uint8_t *, size_t, uint64_t, bool, uint16_t *) { return {}; }
bool is_returning(DisasmState &) { return false; }
#define require(value)                                                            \
    do {                                                                          \
        if (!(value)) {                                                           \
            std::cerr << "Failed at line " << __LINE__ << ": " << #value << "\n"; \
            std::abort();                                                         \
        }                                                                         \
    } while (false)
int main(int argc, char **argv) {
    const int count = argc > 1 ? std::atoi(argv[1]) : 1;
    set_ios_jit_threads(count);
    const int cache_mb = argc > 2 ? std::atoi(argv[2]) : 16;
    set_ios_jit_cache_size(cache_mb * 1024 * 1024);
    set_ios_guest_memory_limit(512ULL * 1024 * 1024);
    MemState memory;
    require(init(memory, false));
    require(alloc_at(memory, 0x10000, 4096, "test code") == 0x10000);
    uint32_t instructions[] = { 0xe2800001, 0xee1d1f70, 0xef000000 }; // add r0,1; read TPIDRURO into r1; svc
    std::memcpy(Ptr<void>(0x10000).get(memory), instructions, sizeof(instructions));
    std::vector<CPUStatePtr> guests;
    for (int i = 0; i < 12; ++i) {
        guests.push_back(init_cpu(true, i + 1, i, memory));
        require(static_cast<bool>(guests.back()));
        write_cpsr(*guests.back(), 0x10);
        write_reg(*guests.back(), 0, i * 1000);
        write_tpidruro(*guests.back(), 0x100 + i);
        write_float_reg(*guests.back(), 10, 0.25f + i);
    }
    auto exercise = [&](int increment) {
        std::vector<std::thread> host;
        for (int i = 0; i < 12; ++i)
            host.emplace_back([&, i] {
                auto &cpu = *guests[i];
                for (int n = 0; n < 100; ++n) {
                    const auto before = read_reg(cpu, 0);
                    write_pc(cpu, 0x10000);
                    // Like ThreadState::run_loop, continue after cache/stop yields.
                    // A bounded retry count detects a stuck slot or stale halt state.
                    for (int slice = 0; slice < 100; ++slice) {
                        require(run(cpu) == 0);
                        if (cpu.svc_called)
                            break;
                    }
                    require(cpu.svc_called);
                    require(read_reg(cpu, 0) == before + increment);
                    require(read_reg(cpu, 1) == static_cast<unsigned>(0x100 + i));
                    require(read_float_reg(cpu, 10) == 0.25f + i);
                }
            });
        for (auto &t : host)
            t.join();
    };
    exercise(1);
    *Ptr<uint32_t>(0x10000).get(memory) = 0xe2800002;
    // Kernel invalidation fans out through every guest sharing this pool.
    // Queue duplicate and disjoint intervals before any worker resumes.
    for (auto &guest : guests) {
        invalidate_jit_cache(*guest, 0x10000, 4);
        invalidate_jit_cache(*guest, 0x10200, 4);
    }
    exercise(2);
    // Overflow fallback must also invalidate already compiled instructions.
    *Ptr<uint32_t>(0x10000).get(memory) = 0xe2800005;
    invalidate_jit_cache(*guests.front(), UINT32_MAX, 8);
    exercise(5);
    *Ptr<uint32_t>(0x10000).get(memory) = 0xeafffffe; // infinite branch must yield a time slice
    invalidate_jit_cache(*guests.front(), 0x10000, 4);
    write_pc(*guests.front(), 0x10000);
    auto spinning = std::async(std::launch::async, [&] { return run(*guests.front()); });
    require(spinning.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    require(spinning.get() == 0);
    // A queued stop must be consumed without executing another guest instruction.
    stop(*guests.front());
    const auto stopped_pc = read_pc(*guests.front());
    require(run(*guests.front()) == 0 && !guests.front()->svc_called);
    require(read_pc(*guests.front()) == stopped_pc);
    // Step a different guest after a shared worker ran an infinite branch.
    *Ptr<uint32_t>(0x10000).get(memory) = 0xe2800003;
    invalidate_jit_cache(*guests.front(), 0x10000, 4);
    auto &stepped = *guests.back();
    write_pc(stepped, 0x10000);
    const auto old_r0 = read_reg(stepped, 0);
    for (int n = 0; n < 10 && read_pc(stepped) == 0x10000; ++n)
        require(step(stepped) == 0);
    require(read_pc(stepped) == 0x10004 && read_reg(stepped, 0) == old_r0 + 3);
    require(!hit_breakpoint(stepped));
    // Exercise the same signal-context stop boundary as kubridge using a real
    // protected guest load. No pool/state mutex may be acquired by this stop.
    require(alloc_at(memory, 0x20000, 4096, "protected data") == 0x20000);
    *Ptr<uint32_t>(0x20000).get(memory) = 1234;
    uint32_t fault_code[] = { 0xe5923000, 0xef000000 }; // ldr r3,[r2]; svc
    std::memcpy(Ptr<void>(0x10000).get(memory), fault_code, sizeof(fault_code));
    invalidate_jit_cache(stepped, 0x10000, sizeof(fault_code));
    std::atomic_bool fault_seen{ false };
    require(add_protect(memory, 0x20000, 4096, MemPerm::None, [&](Address, bool) {
        fault_seen.store(true);
        stop_from_signal(stepped);
        return true;
    }));
    write_reg(stepped, 2, 0x20000);
    write_pc(stepped, 0x10000);
    for (int n = 0; n < 100; ++n) {
        require(run(stepped) == 0);
        if (stepped.svc_called)
            break;
    }
    require(fault_seen.load() && stepped.svc_called && read_reg(stepped, 3) == 1234);
    free(memory, 0x20000);
    std::memcpy(Ptr<void>(0x10000).get(memory), instructions, sizeof(instructions));
    guests.clear(); // Must destroy pooled caches before the memory arena.
    // A fresh session in the same arena must not retain old translated code.
    *Ptr<uint32_t>(0x10000).get(memory) = 0xe2800004;
    auto restarted = init_cpu(true, 100, 0, memory);
    require(static_cast<bool>(restarted));
    write_cpsr(*restarted, 0x10);
    write_pc(*restarted, 0x10000);
    require(run(*restarted) == 0 && restarted->svc_called);
    require(read_reg(*restarted, 0) == 4);
    restarted.reset();
    // Reject budget overflow before allocating or clearing a large mapping.
    const auto before = memory.allocation_budget.used();
    require(alloc(memory, 512 * 1024 * 1024, "over budget") == 0);
    require(memory.allocation_budget.used() == before);
    const auto small = alloc(memory, 16384, "budget return");
    require(small != 0);
    free(memory, small);
    require(memory.allocation_budget.used() == before);
    const auto aligned = alloc_aligned(memory, 16384, "aligned", 65536);
    require(aligned != 0 && aligned % 65536 == 0);
    free(memory, aligned);
    require(memory.allocation_budget.used() == before);
    require(try_alloc_at(memory, 0x10000, 4096, "occupied") == 0);
    require(memory.allocation_budget.used() == before);
    require(alloc_aligned(memory, UINT32_MAX, "overflow", 4096) == 0);
    require(alloc(memory, UINT32_MAX, "rounding overflow") == 0);
    require(try_alloc_at(memory, 0x20001, UINT32_MAX, "offset overflow") == 0);
    require(memory.allocation_budget.used() == before);
    std::cout << "Actual pooled Dynarmic: slots=" << count << "; register/TLS isolation, invalidation, time slicing and memory budget passed\n";
}
