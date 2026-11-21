// Block-based partition buffers used to shuffle records between stages.
#pragma once
#include <sys/mman.h>

#include "common.hpp"

constexpr size_t BLOCK_BYTES = 1 << 15;
// Blocks are spaced slightly more than their size apart so that the write
// positions of many streams spread over different cache sets instead of all
// colliding at the same offset of power-of-two aligned blocks.
constexpr size_t BLOCK_STRIDE = BLOCK_BYTES + 192;

// Transparent huge pages for shuffle memory (THP=0 turns the advice off).
inline bool g_huge_pages = true;

// One large virtual reservation shared by all threads. Carving blocks out of
// it with an atomic bump pointer replaces many per-thread mmap calls (each
// takes the address-space lock exclusively and stalls every concurrent page
// fault) with a single call before the workers start. Pages are still touched
// lazily, by the thread that uses them.
class SlabSource {
public:
    static SlabSource& get() {
        static SlabSource s;
        return s;
    }
    // Reserves `bytes` of address space; returns false if the system refuses.
    bool reserve(size_t bytes) {
        constexpr size_t ALIGN = 2ull << 20;
        bytes = (bytes + ALIGN - 1) & ~(ALIGN - 1);
        void* m = ::mmap(nullptr, bytes + ALIGN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (m == MAP_FAILED) return false;
        char* a = (char*)(((uintptr_t)m + ALIGN - 1) & ~(ALIGN - 1));
#ifdef MADV_HUGEPAGE
        if (g_huge_pages) ::madvise(a, bytes, MADV_HUGEPAGE);
#endif
        base_ = a;
        size_ = bytes;
        next_.store(0);
        return true;
    }
    // Returns `bytes` (a multiple of 64) or nullptr when the reservation is used up.
    char* take(size_t bytes) {
        if (!base_) return nullptr;
        size_t off = next_.fetch_add(bytes, std::memory_order_relaxed);
        return off + bytes <= size_ ? base_ + off : nullptr;
    }

private:
    char* base_ = nullptr;
    size_t size_ = 0;
    std::atomic<size_t> next_{0};
};

// A large zero-filled region for long-lived tables, 2 MB aligned and (when
// enabled) huge-page backed, so a table of hundreds of MB costs a few hundred
// page faults instead of a hundred thousand. Never freed: the process exits
// right after writing its output.
inline void* huge_region(size_t bytes) {
    constexpr size_t ALIGN = 2ull << 20;
    bytes = (bytes + ALIGN - 1) & ~(ALIGN - 1);
    if (char* p = SlabSource::get().take(bytes)) return p;
    void* m = ::mmap(nullptr, bytes + ALIGN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) throw std::bad_alloc();
    char* a = (char*)(((uintptr_t)m + ALIGN - 1) & ~(ALIGN - 1));
#ifdef MADV_HUGEPAGE
    if (g_huge_pages) ::madvise(a, bytes, MADV_HUGEPAGE);
#endif
    return a;
}

// Per-thread block allocator with a free list so blocks are recycled
// between stages and peak memory stays near one stage's data.
class BlockPool {
public:
    ~BlockPool() {
        for (auto& s : slabs_) ::munmap(s.first, s.second);
    }
    char* get() {
        if (!free_.empty()) {
            char* b = free_.back();
            free_.pop_back();
            return b;
        }
        if (cur_ + BLOCK_BYTES > end_) grow();
        char* b = cur_;
        cur_ += BLOCK_STRIDE;
        return b;
    }
    void put(char* b) { free_.push_back(b); }

private:
    static constexpr size_t GRANULE = 4ull << 20;  // taken from the shared reservation
    static constexpr size_t SLAB = 64ull << 20;    // fallback private mapping
    char *cur_ = nullptr, *end_ = nullptr;
    std::vector<char*> free_;
    std::vector<std::pair<void*, size_t>> slabs_;
    void grow() {
        if (char* g = SlabSource::get().take(GRANULE)) {
            cur_ = g;
            end_ = g + GRANULE;
            return;
        }
        void* m = ::mmap(nullptr, SLAB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (m == MAP_FAILED) throw std::bad_alloc();
#ifdef MADV_HUGEPAGE
        if (g_huge_pages) ::madvise(m, SLAB, MADV_HUGEPAGE);
#endif
        slabs_.emplace_back(m, SLAB);
        cur_ = (char*)m;
        end_ = cur_ + SLAB;
    }
};

// Append-only record stream made of pool blocks.
template <class T>
class Stream {
public:
    static constexpr size_t CAP = BLOCK_BYTES / sizeof(T);
    static constexpr size_t PF_AHEAD = 128;
    T* alloc(BlockPool& pool) {
        if (cur_ == lim_) {
            T* b = reinterpret_cast<T*>(pool.get());
            blocks_.push_back(b);
            cur_ = b;
            lim_ = b + CAP;
        }
        // Streams outnumber what the hardware prefetcher can follow, so pull
        // the lines ahead of the write position into cache explicitly.
        __builtin_prefetch(reinterpret_cast<const char*>(cur_) + PF_AHEAD, 1);
        return cur_++;
    }
    size_t size() const { return blocks_.empty() ? 0 : (blocks_.size() - 1) * CAP + (size_t)(cur_ - blocks_.back()); }
    // Calls f(const T*, count) for each block in order.
    template <class F>
    void for_each_block(F&& f) const {
        for (size_t i = 0; i < blocks_.size(); ++i)
            f(static_cast<const T*>(blocks_[i]), (i + 1 == blocks_.size()) ? (size_t)(cur_ - blocks_[i]) : CAP);
    }
    // Returns all blocks to the pool (records become invalid).
    void release(BlockPool& pool) {
        for (T* b : blocks_) pool.put(reinterpret_cast<char*>(b));
        blocks_.clear();
        cur_ = lim_ = nullptr;
    }
    // Appends all records to out and returns the blocks to the pool.
    void drain_into(std::vector<T>& out, BlockPool& pool) {
        for (size_t i = 0; i < blocks_.size(); ++i) {
            size_t n = (i + 1 == blocks_.size()) ? (size_t)(cur_ - blocks_[i]) : CAP;
            out.insert(out.end(), blocks_[i], blocks_[i] + n);
            pool.put(reinterpret_cast<char*>(blocks_[i]));
        }
        blocks_.clear();
        cur_ = lim_ = nullptr;
    }

private:
    T* cur_ = nullptr;
    T* lim_ = nullptr;
    std::vector<T*> blocks_;
};
