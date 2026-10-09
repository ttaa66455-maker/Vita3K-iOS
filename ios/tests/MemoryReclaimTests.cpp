#include <vita3k_ios/MemoryReclaim.h>

#include <cassert>
#include <thread>
#include <vector>

int main() {
    using vita3k_ios::MemoryReclaimRequests;
    using namespace std::chrono_literals;
    constexpr uint64_t mib = 1024 * 1024;
    const auto start = MemoryReclaimRequests::Clock::time_point{};
    MemoryReclaimRequests requests;
    assert(!requests.consume(start, 400 * mib));
    assert(requests.request());
    assert(!requests.request());
    assert(requests.consume(start, 400 * mib)); // First OS warning is immediate.
    assert(!requests.has_pending());
    assert(requests.request());
    assert(!requests.consume(start + 999ms, 400 * mib));
    assert(requests.has_pending()); // Last request in the burst is not discarded.
    assert(requests.consume(start + 1s, 400 * mib));
    assert(!requests.consume(start + 2s, 400 * mib));

    // Headroom collapses during the cooldown: respond in this frame.
    assert(requests.request());
    assert(!requests.consume(start + 1100ms, 181 * mib));
    assert(requests.consume(start + 1101ms, 180 * mib));
    assert(requests.request());
    assert(requests.consume(start + 1102ms, 80 * mib));
    assert(requests.request());
    assert(requests.consume(start + 1103ms, 0)); // Unknown OS reading is conservative.

    // Recovered headroom restores the cooldown, with an outstanding request.
    assert(requests.request());
    assert(!requests.consume(start + 1104ms, 500 * mib));
    assert(requests.consume(start + 2103ms, 500 * mib));

    // Multiple OS/proactive producers may queue a burst concurrently.
    std::vector<std::thread> producers;
    for (int i = 0; i < 8; ++i)
        producers.emplace_back([&] {
            for (int n = 0; n < 1000; ++n)
                requests.request();
        });
    for (auto &producer : producers)
        producer.join();
    assert(requests.consume(start + 4s, 400 * mib));
    assert(!requests.consume(start + 4s, 400 * mib));
    // A later producer must still be visible after the prior consume.
    std::thread later([&] { requests.request(); });
    later.join();
    assert(requests.has_pending());
    assert(requests.consume(start + 5s, 400 * mib));

    // Sustained warnings at 10 Hz: bounded work, followed by a trailing reclaim.
    MemoryReclaimRequests storm;
    int reclaimed = 0;
    for (int i = 0; i < 100; ++i) {
        storm.request();
        reclaimed += storm.consume(start + i * 100ms, 400 * mib);
    }
    assert(reclaimed == 10);
    assert(storm.has_pending());
    assert(storm.consume(start + 10s, 400 * mib));
    assert(!storm.has_pending());
}
