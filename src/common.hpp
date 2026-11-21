// Shared types, constants and small threading helpers.
#pragma once
#include <sys/resource.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <exception>
#include <functional>
#include <fstream>
#include <mutex>
#include <sched.h>
#include <time.h>
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

// Per-worker accounting: on-CPU time versus elapsed time of each worker
// thread, accumulated between two Profile marks. The difference is time the
// worker spent blocked (file I/O, locks, page-fault waits) or descheduled.
struct ThreadStats {
    std::atomic<u64> cpu_ns[256], wall_ns[256], read_ns[256];
    ThreadStats() {
        for (int i = 0; i < 256; ++i) cpu_ns[i] = wall_ns[i] = read_ns[i] = 0;
    }
};
inline ThreadStats& thread_stats() {
    static ThreadStats s;
    return s;
}
inline u64 thread_cpu_ns() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}
inline u64 mono_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

// Stage timings and counters, printed to stderr when finished.
struct Profile {
    Stopwatch total, stage;
    bool verbose = true;
    void mark(const char* name) {
        double s = stage.lap();
        if (!verbose) return;
        struct rusage ru;
        ::getrusage(RUSAGE_SELF, &ru);
        auto sec = [](const timeval& v) { return (double)v.tv_sec + (double)v.tv_usec * 1e-6; };
        std::fprintf(stderr, "STAGE %-22s %8.3fs  (faults %ld, sys %.3fs)\n", name, s, ru.ru_minflt - last.minflt,
                     sec(ru.ru_stime) - last.sys);
        // Worker view of the same stage.
        ThreadStats& ts = thread_stats();
        double cpu_sum = 0, cpu_max = 0, wall_sum = 0, wall_max = 0, read_sum = 0;
        int n = 0;
        for (int i = 0; i < 256; ++i) {
            u64 w = ts.wall_ns[i].exchange(0), c = ts.cpu_ns[i].exchange(0), r = ts.read_ns[i].exchange(0);
            if (!w) continue;
            ++n;
            cpu_sum += (double)c * 1e-9;
            cpu_max = std::max(cpu_max, (double)c * 1e-9);
            wall_sum += (double)w * 1e-9;
            wall_max = std::max(wall_max, (double)w * 1e-9);
            read_sum += (double)r * 1e-9;
        }
        std::fprintf(stderr,
                     "DIAG  %-22s user %.3f sys %.3f | majflt %ld | ctxsw vol %ld invol %ld | workers %d: busy-cpu avg "
                     "%.3f max %.3f, elapsed avg %.3f max %.3f, off-cpu avg %.3f, in-read avg %.3f\n",
                     name, sec(ru.ru_utime) - last.user, sec(ru.ru_stime) - last.sys, ru.ru_majflt - last.majflt,
                     ru.ru_nvcsw - last.vcsw, ru.ru_nivcsw - last.ivcsw, n, n ? cpu_sum / n : 0, cpu_max,
                     n ? wall_sum / n : 0, wall_max, n ? (wall_sum - cpu_sum) / n : 0, n ? read_sum / n : 0);
        // Resident memory and how much of it is backed by transparent huge pages.
        long rss_kb = 0, thp_kb = 0;
        if (FILE* f = std::fopen("/proc/self/smaps_rollup", "r")) {
            char line[256];
            while (std::fgets(line, sizeof line, f)) {
                std::sscanf(line, "Rss: %ld kB", &rss_kb);
                std::sscanf(line, "AnonHugePages: %ld kB", &thp_kb);
            }
            std::fclose(f);
        }
        std::fprintf(stderr, "DIAG  %-22s rss %ld MB, of which huge-page backed %ld MB\n", name, rss_kb / 1024,
                     thp_kb / 1024);
        last = {ru.ru_minflt, ru.ru_majflt, ru.ru_nvcsw, ru.ru_nivcsw, sec(ru.ru_utime), sec(ru.ru_stime)};
    }
    struct Snap {
        long minflt = 0, majflt = 0, vcsw = 0, ivcsw = 0;
        double user = 0, sys = 0;
    } last;
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

// Persistent workers for the main thread's parallel sections. Creating and
// destroying threads per stage costs mmap/munmap of stacks (exclusive address
// space locks plus TLB shootdowns); reusing them avoids that.
class WorkerPool {
public:
    static WorkerPool& get() {
        static WorkerPool* p = new WorkerPool;  // never destroyed: workers outlive static teardown
        return *p;
    }
    bool owned_by_caller() const { return owner_ == std::this_thread::get_id(); }
    // Runs fn(t) for t in [0, threads) with the caller as thread 0.
    void run(int threads, const std::function<void(int)>& fn) {
        while ((int)workers_.size() < threads - 1) {
            int id = (int)workers_.size() + 1;
            const u64 g0 = generation_;  // only the owner changes it, so this is race-free
            workers_.emplace_back([this, id, g0] { loop(id, g0); });
            workers_.back().detach();
        }
        {
            std::lock_guard<std::mutex> g(m_);
            job_ = &fn;
            active_ = threads;
            remaining_ = threads - 1;
            ++generation_;
        }
        cv_work_.notify_all();
        fn(0);
        std::unique_lock<std::mutex> g(m_);
        cv_done_.wait(g, [&] { return remaining_ == 0; });
        job_ = nullptr;
    }

private:
    WorkerPool() : owner_(std::this_thread::get_id()) {}
    void loop(int id, u64 seen) {
        for (;;) {
            const std::function<void(int)>* job;
            {
                std::unique_lock<std::mutex> g(m_);
                cv_work_.wait(g, [&] { return generation_ != seen; });
                seen = generation_;
                if (id >= active_) continue;
                job = job_;
            }
            (*job)(id);
            std::lock_guard<std::mutex> g(m_);
            if (--remaining_ == 0) cv_done_.notify_one();
        }
    }
    std::thread::id owner_;
    std::mutex m_;
    std::condition_variable cv_work_, cv_done_;
    std::vector<std::thread> workers_;
    const std::function<void(int)>* job_ = nullptr;
    int active_ = 0, remaining_ = 0;
    u64 generation_ = 0;
};
inline std::thread::id& pool_owner_slot() {
    static std::thread::id id = std::this_thread::get_id();  // first call is made by main
    return id;
}

// Runs fn(thread_id) on `threads` threads and rethrows the first failure.
// The main thread's calls use the persistent pool; calls made from other
// threads (the background dictionary build) spawn their own workers.
template <class F>
void run_threads(int threads, F&& fn) {
    std::exception_ptr first;
    std::mutex lock;
    auto body = [&](int t) {
        const u64 w0 = mono_ns(), c0 = thread_cpu_ns();
        try {
            fn(t);
            thread_stats().wall_ns[t & 255].fetch_add(mono_ns() - w0, std::memory_order_relaxed);
            thread_stats().cpu_ns[t & 255].fetch_add(thread_cpu_ns() - c0, std::memory_order_relaxed);
        } catch (...) {
            std::lock_guard<std::mutex> g(lock);
            if (!first) first = std::current_exception();
        }
    };
    if (threads <= 1) {
        body(0);
    } else if (std::this_thread::get_id() == pool_owner_slot()) {
        WorkerPool::get().run(threads, std::function<void(int)>(body));
    } else {
        std::vector<std::thread> pool;
        for (int t = 1; t < threads; ++t) pool.emplace_back(body, t);
        body(0);
        for (auto& th : pool) th.join();
    }
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

// Optional cycle probes: build with -DSS_PROFILE and read the CYC lines.
#ifdef SS_PROFILE
#include <x86intrin.h>
inline u64 g_cycles[32];
#define PROBE_BEGIN(v) const u64 v = __rdtsc()
#define PROBE_END(v, slot) (g_cycles[slot] += __rdtsc() - (v))
#else
#define PROBE_BEGIN(v) (void)0
#define PROBE_END(v, slot) (void)0
#endif
