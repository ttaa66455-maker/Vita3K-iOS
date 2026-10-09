#include <cpu/jit_invalidation.h>

#include <cassert>
#include <limits>
#include <utility>
#include <vector>

int main() {
    cpu::JitInvalidation pending;
    std::vector<std::pair<uint32_t, std::size_t>> invalidated;
    int clears = 0;
    auto drain = [&] {
        invalidated.clear();
        pending.drain([&](uint32_t start, std::size_t length) { invalidated.emplace_back(start, length); },
            [&] { ++clears; });
    };
    pending.add(100, 4);
    for (int guest = 0; guest < 100; ++guest)
        pending.add(100, 4); // Kernel fan-out must not exhaust the bounded queue.
    pending.add(108, 4);
    pending.add(104, 4); // Bridge two existing ranges.
    pending.add(102, 2);
    drain();
    assert(clears == 0 && invalidated.size() == 1);
    assert(invalidated[0].first == 100 && invalidated[0].second == 12);
    drain();
    assert(clears == 0 && invalidated.empty());

    pending.add(200, 4);
    pending.add(300, 4);
    drain();
    assert(clears == 0 && invalidated.size() == 2);
    for (auto [start, length] : invalidated)
        assert((start == 200 || start == 300) && length == 4);

    pending.add(0, 0);
    drain();
    assert(clears == 0 && invalidated.empty());
    for (uint32_t i = 0; i < 33; ++i)
        pending.add(i * 16, 4);
    drain();
    assert(clears == 1 && invalidated.empty());
    pending.add(UINT32_MAX, 4);
    drain();
    assert(clears == 2 && invalidated.empty());
    pending.add(1, std::numeric_limits<std::size_t>::max());
    drain();
    assert(clears == 3 && invalidated.empty());
    pending.add(100, 4); // Normal operation resumes after full fallback.
    drain();
    assert(clears == 3 && invalidated.size() == 1);

    // Invalidate only modified translations; preserve unrelated hot blocks.
    std::array<bool, 4096> cached;
    cached.fill(true);
    pending.add(128, 12);
    pending.add(1024, 4);
    pending.drain([&](uint32_t start, std::size_t length) {
        for (std::size_t i = start; i < start + length; ++i)
            cached[i] = false; }, [&] { cached.fill(false); });
    for (std::size_t i = 0; i < cached.size(); ++i)
        assert(cached[i] == !((i >= 128 && i < 140) || (i >= 1024 && i < 1028)));
}
