// Minimal Parquet writer: uncompressed, PLAIN encoding, one data page per
// column chunk. Supports INT32/INT64/BYTE_ARRAY, optional columns, and the
// STRING and DATE logical types.
#pragma once
#include <fcntl.h>
#include <unistd.h>

#include "common.hpp"

namespace pq {

enum Type { INT32 = 1, INT64 = 2, BYTE_ARRAY = 6 };
enum Logical { PLAIN_TYPE = 0, STRING = 1, DATE = 2 };

struct ColumnSpec {
    const char* name;
    Type type;
    bool optional;
    Logical logical;
};

// Thrift compact protocol writer (only what the footer needs).
class Thrift {
public:
    std::vector<u8> buf;
    void put(u8 v) { buf.push_back(v); }
    void varint(u64 v) {
        while (v >= 0x80) {
            put((u8)(v | 0x80));
            v >>= 7;
        }
        put((u8)v);
    }
    void zigzag(i64 v) { varint((u64)((v << 1) ^ (v >> 63))); }
    void begin() {
        stack_.push_back(cur_);
        cur_ = 0;
    }
    void end() {
        put(0);
        cur_ = stack_.back();
        stack_.pop_back();
    }
    void field(int id, int type) {
        int d = id - cur_;
        if (d > 0 && d <= 15) {
            put((u8)((d << 4) | type));
        } else {
            put((u8)type);
            zigzag(id);
        }
        cur_ = id;
    }
    void i32(int id, i64 v) {
        field(id, 5);
        zigzag(v);
    }
    void i64f(int id, i64 v) {
        field(id, 6);
        zigzag(v);
    }
    void str(int id, std::string_view s) {
        field(id, 8);
        raw_str(s);
    }
    void raw_str(std::string_view s) {
        varint(s.size());
        buf.insert(buf.end(), s.begin(), s.end());
    }
    void list(int id, int elem, size_t n) {
        field(id, 9);
        if (n < 15) {
            put((u8)((n << 4) | elem));
        } else {
            put((u8)(0xF0 | elem));
            varint(n);
        }
    }
    void struct_field(int id) {
        field(id, 12);
        begin();
    }

private:
    std::vector<int> stack_;
    int cur_ = 0;
};

class File {
public:
    File(std::string path, std::vector<ColumnSpec> cols) : path_(std::move(path)), spec_(std::move(cols)), col_(spec_.size()) {}
    ~File() {
        if (fd_ >= 0) ::close(fd_);
    }

    void add_i32(int c, i64 v) {
        i32_t x = (i32_t)v;
        append(col_[c].vals, &x, 4);
    }
    void add_i64(int c, i64 v) { append(col_[c].vals, &v, 8); }
    void add_str(int c, const char* p, size_t n) {
        u32 len = (u32)n;
        append(col_[c].vals, &len, 4);
        append(col_[c].vals, p, n);
        if (spec_[c].optional) col_[c].defs.push_back(1);
    }
    void add_null(int c) { col_[c].defs.push_back(0); }
    void row_done() {
        if (++rows_ >= GROUP_ROWS) flush_group();
    }
    u64 total_rows() const { return total_rows_ + rows_; }

    // Writes remaining rows and the footer. A file is created even when
    // there are no rows if create_empty is set.
    void finish(bool create_empty) {
        if (rows_) flush_group();
        if (fd_ < 0 && !create_empty) return;
        open_if_needed();
        write_footer();
        flush_out();
        ::close(fd_);
        fd_ = -1;
    }

private:
    using i32_t = std::int32_t;
    static constexpr u32 GROUP_ROWS = 1u << 18;
    struct Column {
        std::vector<u8> vals, defs;
    };
    struct ChunkMeta {
        i64 offset, total_size, uncompressed, values;
    };
    struct GroupMeta {
        std::vector<ChunkMeta> chunks;
        i64 bytes, rows;
    };
    std::string path_;
    std::vector<ColumnSpec> spec_;
    std::vector<Column> col_;
    std::vector<GroupMeta> groups_;
    std::vector<u8> out_;
    int fd_ = -1;
    i64 pos_ = 0;
    u64 rows_ = 0, total_rows_ = 0;

    static void append(std::vector<u8>& v, const void* p, size_t n) {
        const u8* b = (const u8*)p;
        v.insert(v.end(), b, b + n);
    }
    void open_if_needed() {
        if (fd_ >= 0) return;
        fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd_ < 0) throw std::runtime_error("cannot create " + path_);
        append(out_, "PAR1", 4);
        pos_ = 4;
    }
    void flush_out() {
        size_t done = 0;
        while (done < out_.size()) {
            ssize_t w = ::write(fd_, out_.data() + done, out_.size() - done);
            if (w < 0) throw std::runtime_error("write failed " + path_);
            done += (size_t)w;
        }
        out_.clear();
    }

    void flush_group() {
        open_if_needed();
        GroupMeta g;
        g.rows = (i64)rows_;
        g.bytes = 0;
        for (size_t c = 0; c < spec_.size(); ++c) {
            Column& col = col_[c];
            std::vector<u8> levels;
            if (spec_[c].optional) {
                size_t groups = (rows_ + 7) / 8;
                Thrift h;
                h.varint((groups << 1) | 1);
                std::vector<u8> packed(groups, 0);
                for (size_t i = 0; i < rows_; ++i)
                    if (col.defs[i]) packed[i >> 3] |= (u8)(1u << (i & 7));
                u32 len = (u32)(h.buf.size() + packed.size());
                append(levels, &len, 4);
                append(levels, h.buf.data(), h.buf.size());
                append(levels, packed.data(), packed.size());
            }
            i64 page = (i64)(levels.size() + col.vals.size());
            Thrift t;
            t.begin();
            t.i32(1, 0);  // DATA_PAGE
            t.i32(2, page);
            t.i32(3, page);
            t.struct_field(5);
            t.i32(1, (i64)rows_);
            t.i32(2, 0);  // PLAIN
            t.i32(3, 3);  // RLE levels
            t.i32(4, 3);
            t.end();
            t.end();
            ChunkMeta m;
            m.offset = pos_;
            append(out_, t.buf.data(), t.buf.size());
            append(out_, levels.data(), levels.size());
            append(out_, col.vals.data(), col.vals.size());
            m.total_size = (i64)t.buf.size() + page;
            m.uncompressed = m.total_size;
            m.values = (i64)rows_;
            pos_ += m.total_size;
            g.bytes += m.total_size;
            g.chunks.push_back(m);
            col.vals.clear();
            col.defs.clear();
        }
        groups_.push_back(std::move(g));
        total_rows_ += rows_;
        rows_ = 0;
        flush_out();
    }

    void write_footer() {
        Thrift t;
        t.begin();
        t.i32(1, 1);  // version
        t.list(2, 12, spec_.size() + 1);
        t.begin();  // root element
        t.str(4, "schema");
        t.i32(5, (i64)spec_.size());
        t.end();
        for (const ColumnSpec& s : spec_) {
            t.begin();
            t.i32(1, s.type);
            t.i32(3, s.optional ? 1 : 0);
            t.str(4, s.name);
            if (s.logical == STRING) {
                t.i32(6, 0);  // UTF8
                t.struct_field(10);
                t.struct_field(1);  // StringType
                t.end();
                t.end();
            } else if (s.logical == DATE) {
                t.i32(6, 6);  // DATE
                t.struct_field(10);
                t.struct_field(6);  // DateType
                t.end();
                t.end();
            }
            t.end();
        }
        t.i64f(3, (i64)total_rows_);
        t.list(4, 12, groups_.size());
        for (const GroupMeta& g : groups_) {
            t.begin();
            t.list(1, 12, g.chunks.size());
            for (size_t c = 0; c < g.chunks.size(); ++c) {
                const ChunkMeta& m = g.chunks[c];
                t.begin();
                t.i64f(2, m.offset);
                t.struct_field(3);  // ColumnMetaData
                t.i32(1, spec_[c].type);
                t.list(2, 5, spec_[c].optional ? 2 : 1);
                t.zigzag(0);
                if (spec_[c].optional) t.zigzag(3);
                t.list(3, 8, 1);
                t.raw_str(spec_[c].name);
                t.i32(4, 0);  // UNCOMPRESSED
                t.i64f(5, m.values);
                t.i64f(6, m.uncompressed);
                t.i64f(7, m.total_size);
                t.i64f(9, m.offset);
                t.end();
                t.end();
            }
            t.i64f(2, g.bytes);
            t.i64f(3, g.rows);
            t.end();
        }
        t.str(6, "shopping-sessions native writer");
        t.end();
        open_if_needed();
        append(out_, t.buf.data(), t.buf.size());
        u32 len = (u32)t.buf.size();
        append(out_, &len, 4);
        append(out_, "PAR1", 4);
    }
};

}  // namespace pq
