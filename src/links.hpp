// Identity links: parallel parse, exact dictionary, union-find and the
// canonical (byte-wise smallest) name of every linked person.
#pragma once
#include <memory>

#include "csv.hpp"
#include "key.hpp"
#include "shuffle.hpp"

struct LinkPair {
    Key a, b;
};

class LinkDict {
public:
    // Maps an ID key to its person root, or NONE32 if the ID is in no link.
    u32 lookup(const Key& k, u64 h) const {
        const Part& p = parts_[part_of(h, qbits_)];
        size_t i = h & p.mask;
        for (;;) {
            const Slot& s = p.slots[i];
            if (s.id == NONE32) return NONE32;
            if (s.k == k) return s.root;
            i = (i + 1) & p.mask;
        }
    }
    const void* slot_address(u64 h) const {
        const Part& p = parts_[part_of(h, qbits_)];
        return &p.slots[h & p.mask];
    }
    // Smallest ID (byte-wise) of the linked person with this root.
    const Key& name_of(u32 root) const { return name_[root]; }
    const void* name_address(u32 root) const { return &name_[root]; }
    size_t ids() const { return keys_.size(); }
    size_t persons() const { return persons_; }

    void build(std::vector<std::vector<LinkPair>>& per_thread, int threads);

private:
    struct alignas(32) Slot {
        Key k;
        u32 id = NONE32;
        u32 root = NONE32;
    };
    struct Part {
        Slot* slots = nullptr;  // carved out of one shared huge-page region
        size_t mask = 0;
        std::vector<Key> keys;  // distinct keys in local id order
        u32 base = 0;
    };
    u32 qbits_ = 0;
    std::vector<Part> parts_;
    std::vector<Key> keys_;           // all distinct IDs by handle
    std::vector<Key> name_;           // root handle -> smallest ID of the person
    size_t persons_ = 0;

    u32 local_id(const Part& p, const Key& k, u64 h) const {
        size_t i = h & p.mask;
        for (;;) {
            const Slot& s = p.slots[i];
            if (s.id == NONE32) return NONE32;
            if (s.k == k) return s.id;
            i = (i + 1) & p.mask;
        }
    }
};

inline void LinkDict::build(std::vector<std::vector<LinkPair>>& per_thread, int threads) {
    const int T = (int)per_thread.size();
    size_t total_pairs = 0;
    for (auto& v : per_thread) total_pairs += v.size();
    const u32 Q = ceil_pow2(std::clamp<size_t>(total_pairs / 20000, 1, 256));
    qbits_ = log2_exact(Q);
    parts_.resize(Q);

    // 1. Scatter every endpoint to the partition of its hash.
    std::vector<std::vector<Key>> scatter((size_t)T * Q);
    run_threads(threads, [&](int t) {
        for (int src = t; src < T; src += threads)
            for (const LinkPair& lp : per_thread[src])
                for (const Key* k : {&lp.a, &lp.b}) scatter[(size_t)src * Q + part_of(hash_key(*k), qbits_)].push_back(*k);
    });

    // 2. Build each partition's table independently.
    // Table sizes first, so all tables can live in a single region.
    {
        size_t total_slots = 0;
        for (u32 q = 0; q < Q; ++q) {
            size_t n = 0;
            for (int t = 0; t < T; ++t) n += scatter[(size_t)t * Q + q].size();
            u32 cap = ceil_pow2(std::max<size_t>(n * 2, 16));
            parts_[q].mask = cap - 1;
            total_slots += cap;
        }
        Slot* region = (Slot*)huge_region(total_slots * sizeof(Slot));
        size_t at = 0;
        for (u32 q = 0; q < Q; ++q) {
            parts_[q].slots = region + at;
            at += parts_[q].mask + 1;
        }
    }
    parallel_jobs(threads, Q, [&](int, size_t q) {
        Part& p = parts_[q];
        for (size_t i = 0; i <= p.mask; ++i) p.slots[i] = Slot{};
        for (int t = 0; t < T; ++t) {
            for (const Key& k : scatter[(size_t)t * Q + q]) {
                u64 h = hash_key(k);
                size_t i = h & p.mask;
                for (;;) {
                    Slot& s = p.slots[i];
                    if (s.id == NONE32) {
                        s.k = k;
                        s.id = (u32)p.keys.size();
                        p.keys.push_back(k);
                        break;
                    }
                    if (s.k == k) break;
                    i = (i + 1) & p.mask;
                }
            }
            std::vector<Key>().swap(scatter[(size_t)t * Q + q]);
        }
    });

    size_t H = 0;
    for (Part& p : parts_) {
        p.base = (u32)H;
        H += p.keys.size();
    }
    keys_.resize(H);
    parallel_jobs(threads, Q, [&](int, size_t q) {
        std::copy(parts_[q].keys.begin(), parts_[q].keys.end(), keys_.begin() + parts_[q].base);
    });

    // 3. Resolve every link to a pair of handles.
    std::vector<std::vector<std::pair<u32, u32>>> edges(T);
    run_threads(threads, [&](int t) {
        for (int src = t; src < T; src += threads) {
            auto& out = edges[src];
            out.reserve(per_thread[src].size());
            for (const LinkPair& lp : per_thread[src]) {
                u64 ha = hash_key(lp.a), hb = hash_key(lp.b);
                const Part& pa = parts_[part_of(ha, qbits_)];
                const Part& pb = parts_[part_of(hb, qbits_)];
                out.emplace_back(pa.base + local_id(pa, lp.a, ha), pb.base + local_id(pb, lp.b, hb));
            }
            std::vector<LinkPair>().swap(per_thread[src]);
        }
    });

    // 4. Union-find over handles.
    std::vector<u32> parent(H), size(H, 1);
    for (u32 i = 0; i < H; ++i) parent[i] = i;
    auto find = [&](u32 x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    for (auto& v : edges) {
        for (auto [a, b] : v) {
            a = find(a);
            b = find(b);
            if (a == b) continue;
            if (size[a] < size[b]) std::swap(a, b);
            parent[b] = a;
            size[a] += size[b];
        }
        std::vector<std::pair<u32, u32>>().swap(v);
    }

    // 5. Flatten roots and pick the byte-wise smallest ID per person.
    std::vector<u32> root(H);
    std::vector<u32> min_of_root(H, NONE32);
    name_.resize(H);
    persons_ = 0;
    for (u32 h = 0; h < H; ++h) {
        u32 r = find(h);
        root[h] = r;
        u32& m = min_of_root[r];
        if (m == NONE32) {
            m = h;
            ++persons_;
        } else if (key_cmp(keys_[h], keys_[m]) < 0) {
            m = h;
        }
    }
    for (u32 h = 0; h < H; ++h)
        if (root[h] == h) name_[h] = keys_[min_of_root[h]];
    parallel_jobs(threads, Q, [&](int, size_t q) {
        Part& p = parts_[q];
        for (size_t i = 0; i <= p.mask; ++i) {
            Slot& s = p.slots[i];
            if (s.id != NONE32) s.root = root[p.base + s.id];
        }
        std::vector<Key>().swap(p.keys);
    });
}
