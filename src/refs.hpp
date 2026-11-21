// Reference data: products, campaigns and the window manifest.
#pragma once
#include <unordered_map>

#include "csv.hpp"
#include "key.hpp"

// Build-once open-addressing map from exact keys to u32 (first insert wins).
class KeyMap {
public:
    void reserve(size_t n) {
        u32 cap = ceil_pow2(std::max<size_t>(n * 2, 16));
        slots_.assign(cap, Slot{});
        mask_ = cap - 1;
    }
    void insert(const Key& k, u32 v) {
        if (slots_.empty() || (count_ + 1) * 2 > slots_.size()) grow();
        size_t i = hash_key(k) & mask_;
        while (slots_[i].v != NONE32) {
            if (slots_[i].k == k) return;
            i = (i + 1) & mask_;
        }
        slots_[i] = Slot{k, v};
        ++count_;
    }
    u32 find(const Key& k) const {
        if (slots_.empty()) return NONE32;
        size_t i = hash_key(k) & mask_;
        while (slots_[i].v != NONE32) {
            if (slots_[i].k == k) return slots_[i].v;
            i = (i + 1) & mask_;
        }
        return NONE32;
    }

private:
    struct Slot {
        Key k;
        u32 v = NONE32;
    };
    std::vector<Slot> slots_;
    size_t mask_ = 0, count_ = 0;
    void grow() {
        std::vector<Slot> old = std::move(slots_);
        u32 cap = ceil_pow2(std::max<size_t>(old.size() * 2, 16));
        slots_.assign(cap, Slot{});
        mask_ = cap - 1;
        count_ = 0;
        for (const Slot& s : old)
            if (s.v != NONE32) insert(s.k, s.v);
    }
};

struct Products {
    KeyMap category_of;                     // product -> category number
    std::vector<std::string> category_name;  // number 0 is "unknown"
    std::vector<u32> category_rank;          // byte-wise rank of each name

    void load(const std::string& path) {
        std::unordered_map<std::string, u32> by_name;
        auto add = [&](std::string_view s) {
            auto it = by_name.find(std::string(s));
            if (it != by_name.end()) return it->second;
            u32 id = (u32)category_name.size();
            category_name.emplace_back(s);
            by_name.emplace(category_name.back(), id);
            return id;
        };
        add("unknown");
        PaddedFile f = read_whole(path);
        Field c[2];
        for_each_small_line(f, [&](const char* p, size_t len) {
            if (!split_line<2>(p, len, c)) return;
            u32 cat = add(std::string_view(c[1].p, c[1].n));
            category_of.insert(make_key(c[0].p, c[0].n), cat);
        });
        std::vector<u32> order(category_name.size());
        for (u32 i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](u32 x, u32 y) { return bytes_cmp(category_name[x], category_name[y]) < 0; });
        category_rank.assign(order.size(), 0);
        for (u32 r = 0; r < order.size(); ++r) category_rank[order[r]] = r;
    }
};

struct Campaigns {
    KeyMap index_of;
    std::vector<std::string> id, channel;
    void load(const std::string& path) {
        PaddedFile f = read_whole(path);
        Field c[2];
        for_each_small_line(f, [&](const char* p, size_t len) {
            if (!split_line<2>(p, len, c)) return;
            u32 ix = (u32)id.size();
            id.emplace_back(c[0].p, c[0].n);
            channel.emplace_back(c[1].p, c[1].n);
            index_of.insert(make_key(c[0].p, c[0].n), ix);
        });
    }
    u32 organic() const { return (u32)id.size(); }
};

struct Window {
    i64 start = 0, end = 0;
};
inline Window load_manifest(const std::string& path) {
    PaddedFile f = read_whole(path);
    std::string_view text(f.data.data(), f.size);
    auto value_of = [&](std::string_view name) -> i64 {
        size_t at = text.find(std::string("\"") + std::string(name) + "\"");
        if (at == std::string_view::npos) throw std::runtime_error("manifest: missing " + std::string(name));
        size_t q1 = text.find('"', text.find(':', at) + 1);
        size_t q2 = q1 == std::string_view::npos ? q1 : text.find('"', q1 + 1);
        if (q2 == std::string_view::npos) throw std::runtime_error("manifest: bad " + std::string(name));
        i64 t;
        if (!parse_ts(text.data() + q1 + 1, q2 - q1 - 1, t)) throw std::runtime_error("manifest: bad time");
        return t;
    };
    return Window{value_of("window_start"), value_of("window_end")};
}
