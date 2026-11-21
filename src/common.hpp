// Shared types, constants and small threading helpers.
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <mutex>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using i64 = std::int64_t;
using u64 = std::uint64_t;
using u32 = std::uint32_t;
using u16 = std::uint16_t;
using u8 = std::uint8_t;

constexpr i64 DAY_MS = 86400000LL;
constexpr i64 WEEK_MS = 7 * DAY_MS;
constexpr i64 SESSION_GAP_MS = 1800000LL;
constexpr i64 NO_TIME = INT64_MIN;
constexpr u32 NONE32 = 0xFFFFFFFFu;

inline i64 floor_div(i64 a, i64 b) {
    i64 q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
inline u32 ceil_pow2(u64 v) {
    u32 p = 1;
    while (p < v) p <<= 1;
    return p;
}
inline u32 log2_exact(u32 pow2) {
    u32 b = 0;
    while ((1u << b) < pow2) ++b;
    return b;
}

struct Stopwatch {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    double lap() {
        auto now = std::chrono::steady_clock::now();
        double s = std::chrono::duration<double>(now - t0).count();
        t0 = now;
        return s;
    }
};

// Stage timings and counters, printed to stderr when finished.
struct Profile {
    Stopwatch total, stage;
    bool verbose = true;
    void mark(const char* name) {
        double s = stage.lap();
        if (verbose) std::fprintf(stderr, "STAGE %-22s %8.3fs\n", name, s);
    }
    void count(const char* name, u64 v) {
        if (verbose) std::fprintf(stderr, "COUNT %-22s %llu\n", name, (unsigned long long)v);
    }
};

// Worker count: NCPU override, else min(affinity, cgroup CPU quota).
inline int detect_threads() {
    if (const char* s = std::getenv("NCPU")) {
        int n = std::atoi(s);
        if (n >= 1) return std::min(n, 256);
    }
    int n = 0;
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) n = CPU_COUNT(&set);
    if (n <= 0) n = (int)std::thread::hardware_concurrency();
    auto read_text = [](const char* path) {
        std::ifstream f(path);
        std::string s;
        std::getline(f, s);
        return s;
    };
    double quota = 0;
    std::string v2 = read_text("/sys/fs/cgroup/cpu.max");
    if (!v2.empty() && v2.compare(0, 3, "max") != 0) {
        double q = 0, p = 0;
        if (std::sscanf(v2.c_str(), "%lf %lf", &q, &p) == 2 && p > 0) quota = q / p;
    } else if (v2.empty()) {
        std::string q = read_text("/sys/fs/cgroup/cpu/cpu.cfs_quota_us");
        std::string p = read_text("/sys/fs/cgroup/cpu/cpu.cfs_period_us");
        if (!q.empty() && !p.empty() && std::atoll(q.c_str()) > 0 && std::atoll(p.c_str()) > 0)
            quota = (double)std::atoll(q.c_str()) / (double)std::atoll(p.c_str());
    }
    if (quota > 0) n = std::min(n, std::max(1, (int)(quota + 0.999)));
    if (n <= 0) n = 8;
    return std::clamp(n, 1, 64);
}

// Runs fn(thread_id) on `threads` threads and rethrows the first failure.
template <class F>
void run_threads(int threads, F&& fn) {
    std::exception_ptr first;
    std::mutex lock;
    auto body = [&](int t) {
        try {
            fn(t);
        } catch (...) {
            std::lock_guard<std::mutex> g(lock);
            if (!first) first = std::current_exception();
        }
    };
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; ++t) pool.emplace_back(body, t);
    body(0);
    for (auto& th : pool) th.join();
    if (first) std::rethrow_exception(first);
}

// Dynamic job distribution: fn(thread_id, job_index) for job in [0, jobs).
template <class F>
void parallel_jobs(int threads, size_t jobs, F&& fn) {
    std::atomic<size_t> next{0};
    run_threads(std::min<size_t>(threads, std::max<size_t>(jobs, 1)), [&](int t) {
        for (;;) {
            size_t j = next.fetch_add(1, std::memory_order_relaxed);
            if (j >= jobs) break;
            fn(t, j);
        }
    });
}
