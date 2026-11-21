// LSD radix sort of 16-byte records by the low bits of a 64-bit key.
#pragma once
#include "common.hpp"

struct KRec {
    u64 key, val;
};

// Sorts a[0, n) by the low `bits` bits of key. Returns the buffer (a or tmp)
// that holds the sorted records. tmp must have room for n records.
inline KRec* radix_sort(KRec* a, KRec* tmp, size_t n, u32 bits) {
    if (n < 2 || bits == 0) return a;
    if (n < 512) {
        const u64 mask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
        std::sort(a, a + n, [mask](const KRec& x, const KRec& y) { return (x.key & mask) < (y.key & mask); });
        return a;
    }
    const u32 passes = (bits + 10) / 11;
    const u32 db = (bits + passes - 1) / passes;
    const u32 radix = 1u << db;
    const u64 dmask = radix - 1;
    thread_local std::vector<u32> hist;
    hist.assign((size_t)passes * radix, 0);
    for (size_t i = 0; i < n; ++i) {
        u64 k = a[i].key;
        for (u32 p = 0; p < passes; ++p) ++hist[(size_t)p * radix + ((k >> (p * db)) & dmask)];
    }
    KRec *src = a, *dst = tmp;
    for (u32 p = 0; p < passes; ++p) {
        u32* h = &hist[(size_t)p * radix];
        if (h[(src[0].key >> (p * db)) & dmask] == n) continue;  // every record shares this digit
        u32 sum = 0;
        for (u32 d = 0; d < radix; ++d) {
            u32 c = h[d];
            h[d] = sum;
            sum += c;
        }
        const u32 sh = p * db;
        for (size_t i = 0; i < n; ++i) dst[h[(src[i].key >> sh) & dmask]++] = src[i];
        std::swap(src, dst);
    }
    return src;
}

// Small slices use insertion sort; larger ones the radix sort.
inline KRec* sort_by_key(KRec* a, KRec* tmp, size_t n, u32 bits) {
    if (n <= 24) {
        for (size_t i = 1; i < n; ++i) {
            KRec x = a[i];
            size_t j = i;
            while (j > 0 && a[j - 1].key > x.key) {
                a[j] = a[j - 1];
                --j;
            }
            a[j] = x;
        }
        return a;
    }
    return radix_sort(a, tmp, n, bits);
}
