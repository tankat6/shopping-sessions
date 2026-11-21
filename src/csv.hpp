// CSV line splitting, field parsers and chunked file reading.
#pragma once
#include <dirent.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>

#include "common.hpp"

#ifdef __AVX2__
#include <immintrin.h>
#endif

// Splits one line (no '\n') into N fields and strips whole-field quotes.
// Returns false for a malformed line: wrong field count, or a quote that is
// not wrapped around a whole field. Needs 64 readable bytes after p+len.
struct Field {
    const char* p;
    u32 n;
};

// Comma positions (relative to the line start) and quote count of one line.
// nc saturates at N, so a line is well formed only when nc == N - 1.
template <int N>
struct Cols {
    size_t comma[N];
    int nc;
    u32 quotes;
};

template <int N>
inline void collect_commas(const char* p, size_t len, Cols<N>& c) {
    c.nc = 0;
    c.quotes = 0;
#ifdef __AVX2__
    const __m256i vc = _mm256_set1_epi8(',');
    const __m256i vq = _mm256_set1_epi8('"');
    for (size_t off = 0; off < len; off += 64) {
        __m256i x0 = _mm256_loadu_si256((const __m256i*)(p + off));
        __m256i x1 = _mm256_loadu_si256((const __m256i*)(p + off + 32));
        u64 cm = (u64)(u32)_mm256_movemask_epi8(_mm256_cmpeq_epi8(x0, vc)) |
                 ((u64)(u32)_mm256_movemask_epi8(_mm256_cmpeq_epi8(x1, vc)) << 32);
        u64 qm = (u64)(u32)_mm256_movemask_epi8(_mm256_cmpeq_epi8(x0, vq)) |
                 ((u64)(u32)_mm256_movemask_epi8(_mm256_cmpeq_epi8(x1, vq)) << 32);
        size_t rem = len - off;
        if (rem < 64) {
            u64 m = (1ull << rem) - 1;
            cm &= m;
            qm &= m;
        }
        c.quotes += (u32)__builtin_popcountll(qm);
        while (cm) {
            if (c.nc < N) c.comma[c.nc] = off + (size_t)__builtin_ctzll(cm);
            c.nc += c.nc < N;
            cm &= cm - 1;
        }
    }
#else
    for (size_t i = 0; i < len; ++i) {
        if (p[i] == ',') {
            if (c.nc < N) c.comma[c.nc] = i;
            c.nc += c.nc < N;
        } else if (p[i] == '"') {
            ++c.quotes;
        }
    }
#endif
}

// Slices the line into fields using precomputed comma positions.
// Quote handling is branch-free: whether a field is quoted is data-dependent
// and close to random, so conditional jumps here would mispredict constantly.
// A field's first byte is never '"' when the field is empty (it is then the
// delimiter), so no emptiness test is needed before looking at it.
template <int N>
inline bool fields_from_cols(const char* p, size_t len, const Cols<N>& c, Field* out) {
    if (len == 0 || c.nc != N - 1) return false;
    size_t wrapped = 0, bad = 0;
    const char* st = p;
    for (int k = 0; k < N; ++k) {
        const char* en = p + ((k == N - 1) ? len : c.comma[k]);
        size_t n = (size_t)(en - st);
        size_t q = (size_t)(st[0] == '"');
        size_t back = (size_t)(n != 0);
        bad |= q & ((size_t)(n < 2) | (size_t)(en[-(long)back] != '"'));
        out[k] = Field{st + q, (u32)(n - 2 * q)};
        wrapped += q;
        st = en + 1;
    }
    return !bad && c.quotes == 2 * wrapped;
}

template <int N>
inline bool split_line(const char* p, size_t len, Field* out) {
    if (len == 0) return false;
    Cols<N> c;
    collect_commas<N>(p, len, c);
    return fields_from_cols<N>(p, len, c, out);
}

inline bool ascii_digit(char c) { return c >= '0' && c <= '9'; }

namespace detail {
inline int two(const char* s, int n) {
    return ascii_digit(s[n]) && ascii_digit(s[n + 1]) ? (s[n] - '0') * 10 + s[n + 1] - '0' : -1;
}
inline bool leap(int y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }
inline int month_days(int y, int m) {
    static const int d[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m >= 1 && m <= 12 ? d[m] + (m == 2 && leap(y)) : 0;
}
}  // namespace detail

constexpr i64 days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (i64)era * 146097 + (i64)doe - 719468;
}
namespace detail {
// Days since the epoch of the first day of each month, 1970..2199.
constexpr int TBL_Y0 = 1970, TBL_YN = 230;
struct MonthStarts {
    std::int32_t d[TBL_YN * 12];
    constexpr MonthStarts() : d() {
        for (int y = 0; y < TBL_YN; ++y)
            for (int m = 0; m < 12; ++m) d[y * 12 + m] = (std::int32_t)days_from_civil(TBL_Y0 + y, (unsigned)m + 1, 1);
    }
};
inline constexpr MonthStarts g_month_starts{};
inline i64 days_ymd(int y, int m, int d) {
    unsigned yi = (unsigned)(y - TBL_Y0);
    if (yi < (unsigned)TBL_YN) return (i64)g_month_starts.d[yi * 12 + (unsigned)(m - 1)] + d - 1;
    return days_from_civil(y, (unsigned)m, (unsigned)d);
}
}  // namespace detail

inline void civil_from_days(i64 z, int& y, int& m, int& d) {
    z += 719468;
    const i64 era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (int)yoe + (int)era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y += (m <= 2);
}

// Reference implementation (scalar). Used as the fallback and by the tests.
inline bool parse_ts_scalar(const char* s, size_t n, i64& t) {
    using namespace detail;
    if (n == 0) return false;
    size_t i = 0;
    while (i < n && ascii_digit(s[i])) ++i;
    if (i == n) {
        i64 v = 0;
        for (size_t k = 0; k < n; ++k) {
            int dg = s[k] - '0';
            if (v > (INT64_MAX - dg) / 10) return false;
            v = v * 10 + dg;
        }
        t = v;
        return true;
    }
    if (n != 20 && n != 25) return false;
    if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':') return false;
    static const int digit_pos[] = {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
    for (int p : digit_pos)
        if (!ascii_digit(s[p])) return false;
    int y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    int m = two(s, 5), d = two(s, 8), h = two(s, 11), mi = two(s, 14), se = two(s, 17);
    if (y < 1 || m < 1 || m > 12 || d < 1 || d > month_days(y, m) || h > 23 || mi > 59 || se > 59)
        return false;
    int off = 0;
    if (n == 20) {
        if (s[19] != 'Z') return false;
    } else {
        if ((s[19] != '+' && s[19] != '-') || s[22] != ':') return false;
        int oh = two(s, 20), om = two(s, 23);
        if (oh < 0 || oh > 23 || om < 0 || om > 59) return false;
        off = (oh * 60 + om) * (s[19] == '+' ? 1 : -1);
    }
    t = days_from_civil(y, (unsigned)m, (unsigned)d) * DAY_MS + (i64)h * 3600000 + (i64)mi * 60000 +
        (i64)se * 1000 - (i64)off * 60000;
    return true;
}


#ifdef __AVX2__
namespace detail {
struct ShiftTable {
    u8 idx[17][16];
    ShiftTable() {
        for (int n = 0; n <= 16; ++n)
            for (int j = 0; j < 16; ++j) {
                int k = j - (16 - n);
                idx[n][j] = k >= 0 ? (u8)k : 0x80;
            }
    }
};
inline const ShiftTable g_shift;

// Digits-only field of 1..16 bytes. Needs 16 readable bytes at s.
inline bool parse_digits16(const char* s, size_t n, u64& v) {
    __m128i d = _mm_sub_epi8(_mm_loadu_si128((const __m128i*)s), _mm_set1_epi8('0'));
    __m128i bad = _mm_subs_epu8(d, _mm_set1_epi8(9));
    u32 ok = (u32)_mm_movemask_epi8(_mm_cmpeq_epi8(bad, _mm_setzero_si128()));
    u32 need = n == 16 ? 0xFFFFu : ((1u << n) - 1);
    if ((ok & need) != need) return false;
    d = _mm_shuffle_epi8(d, _mm_load_si128((const __m128i*)g_shift.idx[n]));
    __m128i t1 = _mm_maddubs_epi16(d, _mm_setr_epi8(10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1));
    __m128i t2 = _mm_madd_epi16(t1, _mm_setr_epi16(100, 1, 100, 1, 100, 1, 100, 1));
    __m128i t3 = _mm_packus_epi32(t2, t2);
    __m128i t4 = _mm_madd_epi16(t3, _mm_setr_epi16(10000, 1, 10000, 1, 10000, 1, 10000, 1));
    u64 x = (u64)_mm_cvtsi128_si64(t4);
    v = (x & 0xFFFFFFFFull) * 100000000ull + (x >> 32);
    return true;
}
}  // namespace detail

// F1 (UTC), F2 (offset) or F3 (epoch millis). Anything else is invalid.
// Needs 32 readable bytes at s.
inline bool parse_ts(const char* s, size_t n, i64& t) {
    using namespace detail;
    if (n == 0) return false;
    if ((n == 20 || n == 25) && s[4] == '-') {
        const __m128i zero = _mm_setzero_si128();
        const __m128i c0 = _mm_set1_epi8('0'), c9 = _mm_set1_epi8(9);
        __m128i a = _mm_loadu_si128((const __m128i*)s);
        __m128i b = _mm_loadu_si128((const __m128i*)(s + 16));
        __m128i da = _mm_sub_epi8(a, c0), db = _mm_sub_epi8(b, c0);
        u32 dm = (u32)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_subs_epu8(da, c9), zero));
        u32 dmb = (u32)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_subs_epu8(db, c9), zero));
        const __m128i pat = _mm_setr_epi8(0, 0, 0, 0, '-', 0, 0, '-', 0, 0, 'T', 0, 0, ':', 0, 0);
        u32 em = (u32)_mm_movemask_epi8(_mm_cmpeq_epi8(a, pat));
        u32 emb = (u32)_mm_movemask_epi8(_mm_cmpeq_epi8(b, _mm_set1_epi8(':')));
        constexpr u32 DIGITS = 0xFu | (3u << 5) | (3u << 8) | (3u << 11) | (3u << 14);
        constexpr u32 PUNCT = (1u << 4) | (1u << 7) | (1u << 10) | (1u << 13);
        // b[0] is s[16] ':' ; b[1], b[2] are the seconds digits.
        if ((dm & DIGITS) != DIGITS || (em & PUNCT) != PUNCT || (emb & 1u) != 1u || (dmb & 6u) != 6u) return false;
        // Gather YYYY MM DD hh mm ss into one vector and fold digit pairs.
        const __m128i ga = _mm_setr_epi8(0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, -128, -128, -128, -128);
        const __m128i gb = _mm_setr_epi8(-128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, -128, 1, 2,
                                         -128, -128);
        __m128i dg = _mm_or_si128(_mm_shuffle_epi8(da, ga), _mm_shuffle_epi8(db, gb));
        __m128i w = _mm_maddubs_epi16(dg, _mm_setr_epi8(10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1));
        u64 lo = (u64)_mm_cvtsi128_si64(w), hi = (u64)_mm_extract_epi64(w, 1);
        int y = (int)((lo & 0xFFFF) * 100 + ((lo >> 16) & 0xFFFF));
        int m = (int)((lo >> 32) & 0xFFFF), dd = (int)(lo >> 48);
        int h = (int)(hi & 0xFFFF), mi = (int)((hi >> 16) & 0xFFFF), se = (int)((hi >> 32) & 0xFFFF);
        if (y < 1 || m < 1 || m > 12 || dd < 1 || h > 23 || mi > 59 || se > 59) return false;
        if (dd > 28 && dd > month_days(y, m)) return false;
        // Zone: 'Z' (n == 20) or +hh:mm / -hh:mm (n == 25), selected without branching.
        const bool is_off = n == 25;
        const char sg = s[19];
        int oh = (s[20] - '0') * 10 + (s[21] - '0'), om = (s[23] - '0') * 10 + (s[24] - '0');
        bool ok_z = sg == 'Z';
        bool ok_off = (sg == '+' || sg == '-') && ((emb >> 6) & 1u) && ((dmb & 0x1B0u) == 0x1B0u) && oh <= 23 && om <= 59;
        if (!(is_off ? ok_off : ok_z)) return false;
        int off = is_off ? (oh * 60 + om) * (sg == '+' ? 1 : -1) : 0;
        t = days_ymd(y, m, dd) * DAY_MS + (i64)h * 3600000 + (i64)mi * 60000 + (i64)se * 1000 - (i64)off * 60000;
        return true;
    }
    if (n <= 16) {
        u64 v;
        if (!parse_digits16(s, n, v)) return false;
        t = (i64)v;
        return true;
    }
    return parse_ts_scalar(s, n, t);
}
#else
inline bool parse_ts(const char* s, size_t n, i64& t) { return parse_ts_scalar(s, n, t); }
#endif

// Digits only, value 1..10,000,000 (leading zeros allowed).
inline bool parse_price(const char* s, size_t n, u32& price) {
    if (n == 0) return false;
    u32 v = 0;
    for (size_t k = 0; k < n; ++k) {
        if (!ascii_digit(s[k])) return false;
        v = v * 10 + (u32)(s[k] - '0');
        if (v > 10000000) return false;
    }
    price = v;
    return v >= 1;
}

inline std::string format_date(i64 day) {
    int y, m, d;
    civil_from_days(day, y, m, d);
    char b[32];
    std::snprintf(b, sizeof b, "%04d-%02d-%02d", y, m, d);
    return b;
}

// Sorted list of regular files named *.csv in dir (size included).
struct InputFile {
    std::string path;
    u64 size;
    int fd = -1;  // opened once by open_inputs; shared by all threads (pread is thread-safe)
};
inline std::vector<InputFile> list_csv(const std::string& dir) {
    std::vector<InputFile> files;
    DIR* d = opendir(dir.c_str());
    if (!d) {
        if (errno == ENOENT) return files;  // a missing folder holds no files
        throw std::runtime_error("cannot open directory " + dir);
    }
    while (dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".csv") != 0) continue;
        std::string path = dir + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        files.push_back({path, (u64)st.st_size});
    }
    closedir(d);
    std::sort(files.begin(), files.end(), [](const InputFile& x, const InputFile& y) { return x.path < y.path; });
    return files;
}

// Opens every file once; chunk readers then use pread on the shared descriptor
// instead of an open/close pair per chunk.
inline void open_inputs(std::vector<InputFile>& files) {
    for (auto& f : files) {
        f.fd = ::open(f.path.c_str(), O_RDONLY | O_CLOEXEC);
        if (f.fd < 0) throw std::runtime_error("cannot open " + f.path);
    }
}

// A byte range of one file; lines that start inside [begin, end) belong to it.
struct Chunk {
    u32 file;
    u64 begin, end;
};
inline std::vector<Chunk> make_chunks(const std::vector<InputFile>& files, u64 chunk_bytes) {
    std::vector<Chunk> out;
    for (u32 f = 0; f < files.size(); ++f) {
        u64 size = files[f].size;
        if (size == 0) continue;
        for (u64 b = 0; b < size; b += chunk_bytes) out.push_back({f, b, std::min(size, b + chunk_bytes)});
    }
    return out;
}

// Read-only mapping of a whole input file (p == nullptr if mapping failed).
struct MappedFile {
    const char* p = nullptr;
    u64 size = 0;
};
inline MappedFile map_file(const InputFile& f) {
    MappedFile m;
    if (f.size == 0) return m;
    int fd = ::open(f.path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return m;
    void* a = ::mmap(nullptr, f.size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (a == MAP_FAILED) return m;
    m.p = (const char*)a;
    m.size = f.size;
    return m;
}

// Feeds the lines of a chunk to fn(ptr, len). Every line is followed by at
// least 128 readable bytes. Uses the mapping when present, else pread.
class ChunkReader {
public:
    ~ChunkReader() { std::free(buf_); }

    // Calls fn(line, len, cols) for every line starting in the chunk. The comma
    // positions come from the same vectorized pass that locates the line ends.
    // fn(line, len, const Cols<N>&).
    template <int N, class F>
    void for_each_record(const InputFile& file, const MappedFile& mf, const Chunk& c, F&& fn) {
        auto slow = [&](const char* p, size_t len) {
            Cols<N> cols;
            collect_commas<N>(p, len, cols);
            fn(p, len, cols);
        };
        if (!mf.p) {
            read_records<N>(file, c, fn);
            return;
        }
#ifdef MADV_POPULATE_READ
        {
            u64 a = c.begin & ~4095ull;
            ::madvise((void*)(mf.p + a), (size_t)(c.end - a), MADV_POPULATE_READ);
        }
#endif
        const char* base = mf.p;
        const u64 size = mf.size;
        u64 pos = c.begin;
        if (c.begin == 0 || base[c.begin - 1] != '\n') {
            const void* nl = std::memchr(base + pos, '\n', (size_t)(size - pos));
            if (!nl) return;
            pos = (u64)((const char*)nl - base) + 1;
        }
        pos = scan_records<N>(base, size, pos, c.end, fn);
        if (pos < c.end) mapped_tail(file, base, size, pos, c.end, slow);
    }

private:
    // Two-step vectorized scan. Step one flattens the comma and newline
    // positions of a batch of 64-byte blocks into index arrays without any
    // data-dependent branches; step two walks the lines using those arrays.
    // Stops where fewer than three blocks remain in the file and returns the
    // start of the first line not yet delivered; the caller finishes the rest
    // with the scalar path.
    template <int N, class F>
    u64 scan_records(const char* base, u64 size, u64 pos, u64 end, F& fn) {
#if defined(__AVX2__) && defined(__BMI__)
        constexpr u64 BATCH = 128 << 10;
        u64 scan_len = BATCH;
        u64 ls = pos;  // absolute start of the first line not delivered
        const __m256i vc = _mm256_set1_epi8(',');
        const __m256i vq = _mm256_set1_epi8('"');
        const __m256i vn = _mm256_set1_epi8('\n');
        while (ls < end) {
            if (ls + 192 > size) return ls;
            // Enough blocks to finish the lines that start before `end`.
            u64 want = std::min<u64>(scan_len, ((end - ls) + 256 + 63) & ~63ull);
            u64 nblk = std::min<u64>(want / 64, (size - 192 - ls) / 64 + 1);
            if (nblk == 0) return ls;
            const size_t cap = (size_t)nblk * 64 + 32;
            if (cidx_.size() < cap) {
                cidx_.resize(cap);
                nidx_.resize(cap);
            }
            if (qpre_.size() < nblk + 1) {
                qpre_.resize(nblk + 1);
                qmask_.resize(nblk + 1);
            }
            u32* cp = cidx_.data();
            u32* np = nidx_.data();
            u32 qacc = 0;
            for (u64 b = 0; b < nblk; ++b) {
                const char* q = base + ls + b * 64;
                __m256i x0 = _mm256_loadu_si256((const __m256i*)q);
                __m256i x1 = _mm256_loadu_si256((const __m256i*)(q + 32));
                u64 cm = (u64)(u32)_mm256_movemask_epi8(_mm256_cmpeq_epi8(x0, vc)) |
                         ((u64)(u32)_mm256_movemask_epi8(_mm256_cmpeq_epi8(x1, vc)) << 32);
                u64 qm = (u64)(u32)_mm256_movemask_epi8(_mm256_cmpeq_epi8(x0, vq)) |
                         ((u64)(u32)_mm256_movemask_epi8(_mm256_cmpeq_epi8(x1, vq)) << 32);
                u64 nm = (u64)(u32)_mm256_movemask_epi8(_mm256_cmpeq_epi8(x0, vn)) |
                         ((u64)(u32)_mm256_movemask_epi8(_mm256_cmpeq_epi8(x1, vn)) << 32);
                qmask_[b] = qm;
                qpre_[b] = qacc;
                qacc += (u32)_mm_popcnt_u64(qm);
                const u32 off = (u32)(b * 64);
                flatten(cp, cm, off, 8);
                flatten(np, nm, off, 2);
            }
            qpre_[nblk] = qacc;
            qmask_[nblk] = 0;
            for (int i = 0; i < 8; ++i) cp[i] = ~0u;
            np[0] = ~0u;
            const size_t nnl = (size_t)(np - nidx_.data());

            // Walk the lines of this batch.
            const u32* cidx = cidx_.data();
            u32 line_rel = 0, qprev = 0;
            size_t ci = 0;
            bool done = false;
            size_t delivered = 0;
            for (size_t k = 0; k < nnl; ++k) {
                const u32 x = nidx_[k];
                const u32 qx = qpre_[x >> 6] + (u32)_mm_popcnt_u64(qmask_[x >> 6] & ((1ull << (x & 63)) - 1));
                Cols<N> cols;
                if (cidx[ci + N - 2] < x && cidx[ci + N - 1] > x) {
                    for (int j = 0; j < N - 1; ++j) cols.comma[j] = cidx[ci + j] - line_rel;
                    cols.nc = N - 1;
                    ci += N - 1;
                } else {
                    int nc = 0;
                    while (cidx[ci] < x) {
                        if (nc < N) cols.comma[nc] = cidx[ci] - line_rel;
                        nc += nc < N;
                        ++ci;
                    }
                    cols.nc = nc;
                }
                cols.quotes = qx - qprev;
                qprev = qx;
                fn(base + ls + line_rel, (size_t)(x - line_rel), (const Cols<N>&)cols);
                line_rel = x + 1;
                ++delivered;
                if (ls + line_rel >= end) {
                    done = true;
                    break;
                }
            }
            ls += line_rel;
            if (done) return ls;
            if (delivered == 0) {  // a line longer than the batch: look further ahead
                if (scan_len >= (1ull << 30)) return ls;
                scan_len *= 2;
            }
        }
        return ls;
#else
        (void)base; (void)size; (void)end; (void)fn;
        return pos;
#endif
    }

#if defined(__AVX2__) && defined(__BMI__)
    // Writes the positions of the set bits of m (plus off) to out and advances
    // out. The first `fixed` slots are written unconditionally; slots beyond
    // the bit count hold garbage that the next write overwrites.
    static inline void flatten(u32*& out, u64 m, u32 off, int fixed) {
        const int cnt = (int)_mm_popcnt_u64(m);
        u64 t = m;
        out[0] = off + (u32)_tzcnt_u64(t);
        t = _blsr_u64(t);
        out[1] = off + (u32)_tzcnt_u64(t);
        t = _blsr_u64(t);
        if (fixed > 2) {
            for (int i = 2; i < 8; ++i) {
                out[i] = off + (u32)_tzcnt_u64(t);
                t = _blsr_u64(t);
            }
        }
        if (cnt > fixed) {  // rare: dense block
            for (int i = fixed; i < cnt; ++i) {
                out[i] = off + (u32)_tzcnt_u64(t);
                t = _blsr_u64(t);
            }
        }
        out += cnt;
    }
    std::vector<u32> cidx_, nidx_, qpre_;
    std::vector<u64> qmask_;
#endif

    template <class F>
    void mapped_tail(const InputFile&, const char* base, u64 size, u64 pos, u64 end, F&& fn) {
        while (pos < end) {
            const char* line = base + pos;
            const char* nl = (const char*)std::memchr(line, '\n', (size_t)(size - pos));
            size_t len = nl ? (size_t)(nl - line) : (size_t)(size - pos);
            if (pos + len + 128 <= size) {
                fn(line, len);
            } else {  // last line of the file: copy so the padding is readable
                ensure(len + PAD);
                std::memcpy(buf_, line, len);
                std::memset(buf_ + len, 0, PAD);
                fn(buf_, len);
            }
            pos += len + 1;
        }
    }

    // Buffered path: one pread covers the chunk plus a margin for the line
    // that straddles its end, then the same vectorized scan runs on the buffer.
    template <int N, class F>
    void read_records(const InputFile& file, const Chunk& c, F& fn) {
        int fd = file.fd;
        struct Closer {
            int fd;
            ~Closer() {
                if (fd >= 0) ::close(fd);
            }
        } closer{-1};
        if (fd < 0) {
            fd = ::open(file.path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) throw std::runtime_error("cannot open " + file.path);
            closer.fd = fd;
        }
        const u64 read_from = c.begin ? c.begin - 1 : 0;  // one byte back tells us if we start on a line
        const u64 want_end = std::min<u64>(file.size, c.end + EXTRA);
        size_t want = (size_t)(want_end - read_from);
        ensure(want + PAD);
        size_t have = fill(fd, read_from, buf_, want);
        size_t pos = 0;
        if (c.begin == 0 || buf_[0] != '\n') {
            // Skip the header (first chunk) or the tail of the previous chunk's last line.
            size_t from = c.begin ? 1 : 0;
            const void* nl = have > from ? std::memchr(buf_ + from, '\n', have - from) : nullptr;
            if (!nl) return;
            pos = (size_t)((const char*)nl - buf_) + 1;
        } else {
            pos = 1;
        }
        // Lines starting before c.end belong here; finish the last one.
        const size_t limit = (size_t)(c.end - read_from);
        pos = scan_records<N>(buf_, have, pos, limit, fn);
        u64 next_off = read_from + have;
        while (pos < limit) {
            const char* nl = (const char*)std::memchr(buf_ + pos, '\n', have - pos);
            if (!nl) {
                if (next_off >= file.size) {  // final line without '\n'
                    emit_slow<N>(buf_ + pos, have - pos, fn);
                    return;
                }
                ensure(have + EXTRA + PAD);
                size_t got = fill(fd, next_off, buf_ + have, EXTRA);
                if (got == 0) {
                    emit_slow<N>(buf_ + pos, have - pos, fn);
                    return;
                }
                have += got;
                next_off += got;
                continue;
            }
            size_t len = (size_t)(nl - (buf_ + pos));
            emit_slow<N>(buf_ + pos, len, fn);
            pos += len + 1;
        }
    }
    template <int N, class F>
    static void emit_slow(const char* p, size_t len, F& fn) {
        Cols<N> cols;
        collect_commas<N>(p, len, cols);
        fn(p, len, (const Cols<N>&)cols);
    }

public:
    u64 read_ns = 0;  // time spent inside pread, for diagnostics

private:
    static constexpr size_t PAD = 128;
    static constexpr size_t EXTRA = 1 << 16;
    char* buf_ = nullptr;
    size_t cap_ = 0;

    void ensure(size_t need) {
        if (need <= cap_) return;
        size_t ncap = std::max(need, cap_ * 2);
        char* nb = (char*)std::realloc(buf_, ncap);
        if (!nb) throw std::bad_alloc();
        buf_ = nb;
        cap_ = ncap;
    }
    // Reads up to n bytes at offset; zeroes the padding after them.
    size_t fill(int fd, u64 off, char* dst, size_t n) {
        size_t total = 0;
        const u64 t0 = mono_ns();
        struct Timer {
            u64& acc;
            u64 t0;
            ~Timer() { acc += mono_ns() - t0; }
        } timer{read_ns, t0};
        while (total < n) {
            ssize_t r = ::pread(fd, dst + total, n - total, (off_t)(off + total));
            if (r < 0) throw std::runtime_error("read failed");
            if (r == 0) break;
            total += (size_t)r;
        }
        std::memset(dst + total, 0, PAD);
        return total;
    }
};

// Whole-file read into a zero-padded buffer (small reference files).
struct PaddedFile {
    std::vector<char> data;  // size() == content + PAD
    size_t size = 0;
};
inline PaddedFile read_whole(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("cannot open " + path);
    PaddedFile f;
    struct stat st;
    size_t cap = (::fstat(fd, &st) == 0 && st.st_size > 0) ? (size_t)st.st_size : 4096;
    f.data.resize(cap + 256);
    for (;;) {
        if (f.size + 128 > f.data.size()) f.data.resize(f.data.size() * 2);
        ssize_t r = ::read(fd, f.data.data() + f.size, f.data.size() - f.size - 128);
        if (r < 0) {
            ::close(fd);
            throw std::runtime_error("read failed " + path);
        }
        if (r == 0) break;
        f.size += (size_t)r;
    }
    ::close(fd);
    std::memset(f.data.data() + f.size, 0, 128);
    return f;
}

// Calls fn(line_ptr, len) for each data line (the header line is skipped).
template <class F>
inline void for_each_small_line(const PaddedFile& file, F&& fn) {
    const char* p = file.data.data();
    const char* end = p + file.size;
    const char* nl = (const char*)std::memchr(p, '\n', file.size);
    if (!nl) return;
    p = nl + 1;
    while (p < end) {
        const char* e = (const char*)std::memchr(p, '\n', (size_t)(end - p));
        size_t len = e ? (size_t)(e - p) : (size_t)(end - p);
        fn(p, len);
        if (!e) break;
        p = e + 1;
    }
}
