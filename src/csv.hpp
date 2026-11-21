// CSV line splitting, field parsers and chunked file reading.
#pragma once
#include <dirent.h>
#include <cerrno>
#include <fcntl.h>
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

template <int N>
inline bool split_line(const char* p, size_t len, Field* out) {
    if (len == 0) return false;
    size_t comma[N];
    int nc = 0;
    u32 quotes = 0;
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
        quotes += (u32)__builtin_popcountll(qm);
        while (cm) {
            if (nc == N - 1) return false;
            comma[nc++] = off + (size_t)__builtin_ctzll(cm);
            cm &= cm - 1;
        }
    }
#else
    for (size_t i = 0; i < len; ++i) {
        if (p[i] == ',') {
            if (nc == N - 1) return false;
            comma[nc++] = i;
        } else if (p[i] == '"') {
            ++quotes;
        }
    }
#endif
    if (nc != N - 1) return false;
    u32 wrapped = 0;
    size_t start = 0;
    for (int k = 0; k < N; ++k) {
        size_t end = (k == N - 1) ? len : comma[k];
        const char* s = p + start;
        size_t n = end - start;
        if (n && s[0] == '"') {
            if (n < 2 || s[n - 1] != '"') return false;
            ++s;
            n -= 2;
            ++wrapped;
        }
        out[k] = Field{s, (u32)n};
        start = end + 1;
    }
    return quotes == 2 * wrapped;
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

inline i64 days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (i64)era * 146097 + (i64)doe - 719468;
}
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

// F1 (UTC), F2 (offset) or F3 (epoch millis). Anything else is invalid.
inline bool parse_ts(const char* s, size_t n, i64& t) {
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

// Reads complete lines for a chunk into a padded buffer and feeds them to fn.
class ChunkReader {
public:
    ~ChunkReader() { std::free(buf_); }

    template <class F>
    void for_each_line(const InputFile& file, const Chunk& c, F&& fn) {
        int fd = ::open(file.path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) throw std::runtime_error("cannot open " + file.path);
        struct Closer {
            int fd;
            ~Closer() { ::close(fd); }
        } closer{fd};
        const u64 read_from = c.begin ? c.begin - 1 : 0;  // one byte back tells us if we start on a line
        size_t want = (size_t)(c.end - read_from);
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
        size_t limit = (size_t)(c.end - read_from);
        u64 next_off = read_from + have;
        while (pos < limit) {
            const char* nl = (const char*)std::memchr(buf_ + pos, '\n', have - pos);
            if (!nl) {
                if (next_off >= file.size) {  // final line without '\n'
                    fn(buf_ + pos, have - pos);
                    return;
                }
                ensure(have + EXTRA + PAD);
                size_t got = fill(fd, next_off, buf_ + have, EXTRA);
                if (got == 0) {
                    fn(buf_ + pos, have - pos);
                    return;
                }
                have += got;
                next_off += got;
                continue;
            }
            size_t len = (size_t)(nl - (buf_ + pos));
            fn(buf_ + pos, len);
            pos += len + 1;
        }
    }

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
