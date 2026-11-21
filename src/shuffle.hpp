// Block-based partition buffers used to shuffle records between stages.
#pragma once
#include <sys/mman.h>

#include "common.hpp"

constexpr size_t BLOCK_BYTES = 1 << 15;

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
        if (cur_ == end_) grow();
        char* b = cur_;
        cur_ += BLOCK_BYTES;
        return b;
    }
    void put(char* b) { free_.push_back(b); }

private:
    static constexpr size_t SLAB = 64ull << 20;
    char *cur_ = nullptr, *end_ = nullptr;
    std::vector<char*> free_;
    std::vector<std::pair<void*, size_t>> slabs_;
    void grow() {
        void* m = ::mmap(nullptr, SLAB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (m == MAP_FAILED) throw std::bad_alloc();
#ifdef MADV_HUGEPAGE
        ::madvise(m, SLAB, MADV_HUGEPAGE);
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
    T* alloc(BlockPool& pool) {
        if (cur_ == lim_) {
            T* b = reinterpret_cast<T*>(pool.get());
            blocks_.push_back(b);
            cur_ = b;
            lim_ = b + CAP;
        }
        return cur_++;
    }
    size_t size() const { return blocks_.empty() ? 0 : (blocks_.size() - 1) * CAP + (size_t)(cur_ - blocks_.back()); }
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
