// Exact 16-byte identifier keys. IDs up to 15 bytes are stored inline;
// longer IDs get a dense number from a locked side table. Equality is
// always exact, so there are no hash-only collisions anywhere.
#pragma once
#include <deque>
#include <immintrin.h>
#include <unordered_map>

#include "common.hpp"

struct Key {
    u64 a = 0, b = 0;
    bool operator==(const Key& o) const { return a == o.a && b == o.b; }
    bool operator!=(const Key& o) const { return !(*this == o); }
};
constexpr u64 TAG_LONG = 0xFFull << 56;  // a = long-string number
constexpr u64 TAG_LINK = 0xFEull << 56;  // a = linked person root
inline bool key_is_long(const Key& k) { return (k.b >> 56) == 0xFF; }

class LongStrings {
public:
    Key intern(std::string_view s) {
        std::lock_guard<std::mutex> g(lock_);
        auto it = index_.find(std::string(s));
        if (it != index_.end()) return Key{it->second, TAG_LONG};
        u32 id = (u32)strings_.size();
        strings_.emplace_back(s);
        index_.emplace(strings_.back(), id);
        return Key{id, TAG_LONG};
    }
    std::string_view get(u32 id) const { return strings_[id]; }

private:
    std::mutex lock_;
    std::deque<std::string> strings_;
    std::unordered_map<std::string, u32> index_;
};
inline LongStrings& long_strings() {
    static LongStrings s;
    return s;
}

namespace detail {
struct alignas(16) Tables {
    u8 mask[16][16];
    u8 len[16][16];
    Tables() {
        std::memset(mask, 0, sizeof mask);
        std::memset(len, 0, sizeof len);
        for (int n = 0; n < 16; ++n) {
            for (int i = 0; i < n; ++i) mask[n][i] = 0xFF;
            len[n][15] = (u8)n;
        }
    }
};
inline const Tables& tables() {
    static const Tables t;
    return t;
}
}  // namespace detail

// Requires 16 readable bytes at p.
inline Key make_key(const char* p, size_t n) {
    if (n <= 15) {
        const auto& t = detail::tables();
        __m128i v = _mm_loadu_si128((const __m128i*)p);
        v = _mm_and_si128(v, _mm_load_si128((const __m128i*)t.mask[n]));
        v = _mm_or_si128(v, _mm_load_si128((const __m128i*)t.len[n]));
        Key k;
        _mm_storeu_si128((__m128i*)&k, v);
        return k;
    }
    return long_strings().intern(std::string_view(p, n));
}

inline u64 hash_key(const Key& k) {
    u64 h = (k.a ^ 0x9E3779B97F4A7C15ull) * 0xFF51AFD7ED558CCDull;
    h ^= h >> 32;
    h = (h + k.b) * 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 29;
#ifdef WEAK_HASH
    h = ((h & 7) << 61) | (h & 7);  // tests: force constant collisions
#endif
    return h;
}
inline u32 part_of(u64 h, u32 bits) { return bits ? (u32)(h >> (64 - bits)) : 0; }

// Copies the key's bytes into tmp (>= 16 bytes) or the long table.
inline std::string_view key_view(const Key& k, char* tmp) {
    if (key_is_long(k)) return long_strings().get((u32)k.a);
    std::memcpy(tmp, &k, 16);
    return std::string_view(tmp, (size_t)(k.b >> 56));
}
inline int bytes_cmp(std::string_view x, std::string_view y) {
    size_t n = std::min(x.size(), y.size());
    int r = n ? std::memcmp(x.data(), y.data(), n) : 0;
    if (r) return r;
    return x.size() == y.size() ? 0 : (x.size() < y.size() ? -1 : 1);
}
// Byte-wise lexicographic order of the original strings.
inline int key_cmp(const Key& x, const Key& y) {
    if (!key_is_long(x) && !key_is_long(y)) {
        u64 xa = __builtin_bswap64(x.a), ya = __builtin_bswap64(y.a);
        if (xa != ya) return xa < ya ? -1 : 1;
        u64 xb = __builtin_bswap64(x.b), yb = __builtin_bswap64(y.b);
        if (xb != yb) return xb < yb ? -1 : 1;
        return 0;
    }
    char t1[16], t2[16];
    return bytes_cmp(key_view(x, t1), key_view(y, t2));
}
