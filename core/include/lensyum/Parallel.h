#pragma once

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

namespace lensyum {

inline int workerCount() {
    const unsigned n = std::thread::hardware_concurrency();
    return static_cast<int>(std::clamp(n == 0 ? 4u : n, 1u, 64u));
}

// Runs fn(i) for i in [0, count) on a set of short-lived threads, handing out indices
// dynamically so uneven work (large blur next to sharp regions) balances itself.
template <class Fn>
void parallelFor(int count, Fn&& fn) {
    if (count <= 0) return;
    const int threads = std::min(workerCount(), count);
    if (threads <= 1) {
        for (int i = 0; i < count; ++i) fn(i);
        return;
    }
    std::atomic<int> next{0};
    auto worker = [&]() {
        for (int i = next.fetch_add(1); i < count; i = next.fetch_add(1)) fn(i);
    };
    std::vector<std::thread> pool;
    pool.reserve(threads - 1);
    for (int t = 0; t < threads - 1; ++t) pool.emplace_back(worker);
    worker();
    for (auto& t : pool) t.join();
}

} // namespace lensyum
