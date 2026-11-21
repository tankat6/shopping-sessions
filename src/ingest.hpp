// Parsing of events and clicks, event dedup and the two shuffles.
#pragma once
#include "csv.hpp"
#include "key.hpp"
#include "links.hpp"
#include "refs.hpp"
#include "shuffle.hpp"

// Event as parsed (one per input line); shuffled by event_id hash.
struct E1 {
    Key ev, user;
    i64 ingest, ts;
    u64 pay;
};
// Surviving event keyed by person; shuffled by person hash.
struct E2 {
    Key pk;
    i64 ts;
    u64 pay;
};
struct C2 {
    Key pk;
    i64 ts;
    u32 camp, pad;
    Key cid;
};

// pay: price bits 0-23, kind bits 24-25 (1 view, 2 cart, 3 purchase),
// valid bit 26, category bits 32-63.
constexpr u64 PAY_VALID = 1ull << 26;
inline u32 pay_price(u64 p) { return (u32)(p & 0xFFFFFF); }
inline u32 pay_kind(u64 p) { return (u32)((p >> 24) & 3); }
inline u32 pay_category(u64 p) { return (u32)(p >> 32); }

struct ThreadData {
    BlockPool pool;
    std::vector<Stream<E1>> s1;
    std::vector<Stream<E2>> s2e;
    std::vector<Stream<C2>> s2c;
    u64 event_lines = 0, bad_lines = 0, click_lines = 0, click_bad = 0, click_valid = 0;
    u64 events_unique = 0, events_valid = 0;
    std::vector<LinkPair> links;
    u64 link_bad = 0;
};

struct IngestContext {
    const Products* products;
    const Campaigns* campaigns;
    const LinkDict* dict;
    Window window;
    u32 p1_bits, p2_bits;
};

inline void parse_event_line(const char* p, size_t len, ThreadData& td, const IngestContext& c) {
    Field f[7];
    if (!split_line<7>(p, len, f) || f[0].n == 0) {
        ++td.bad_lines;
        return;
    }
    ++td.event_lines;
    E1 r;
    r.ev = make_key(f[0].p, f[0].n);
    i64 ts = NO_TIME, ing;
    bool ts_ok = parse_ts(f[2].p, f[2].n, ts) && ts >= c.window.start && ts < c.window.end;
    if (!parse_ingest_f1(f[3].p, f[3].n, ing)) ing = NO_TIME;
    u32 kind = 0, price = 0;
    const Field& ty = f[4];
    if (ty.n == 4 && std::memcmp(ty.p, "view", 4) == 0) kind = 1;
    else if (ty.n == 4 && std::memcmp(ty.p, "cart", 4) == 0) kind = 2;
    else if (ty.n == 8 && std::memcmp(ty.p, "purchase", 8) == 0) {
        kind = 3;
        if (!parse_price(f[6].p, f[6].n, price)) kind = 0;
    }
    bool valid = f[1].n && f[5].n && kind && ts_ok && ing != NO_TIME && ing - ts <= DAY_MS;
    r.ingest = ing;
    r.ts = ts;
    r.user = Key{};
    r.pay = 0;
    if (valid) {
        r.user = make_key(f[1].p, f[1].n);
        u32 cat = 0;
        if (kind == 3) {
            u32 v = c.products->category_of.find(make_key(f[5].p, f[5].n));
            cat = v == NONE32 ? 0 : v;
        }
        r.pay = (u64)price | ((u64)kind << 24) | PAY_VALID | ((u64)cat << 32);
    }
    *td.s1[part_of(hash_key(r.ev), c.p1_bits)].alloc(td.pool) = r;
}

inline void parse_link_line(const char* p, size_t len, ThreadData& td) {
    Field f[2];
    if (!split_line<2>(p, len, f) || f[0].n == 0 || f[1].n == 0) {
        ++td.link_bad;
        return;
    }
    td.links.push_back(LinkPair{make_key(f[0].p, f[0].n), make_key(f[1].p, f[1].n)});
}

struct ClickTmp {
    Key user, cid;
    u64 h;
    i64 ts;
    u32 camp;
};

inline void parse_click_line(const char* p, size_t len, ThreadData& td, const IngestContext& c,
                             std::vector<ClickTmp>& out) {
    Field f[4];
    if (!split_line<4>(p, len, f) || f[0].n == 0) {
        ++td.click_bad;
        return;
    }
    ++td.click_lines;
    if (f[1].n == 0) return;
    u32 camp = c.campaigns->index_of.find(make_key(f[2].p, f[2].n));
    if (camp == NONE32) return;
    i64 ts;
    if (!parse_ts(f[3].p, f[3].n, ts) || ts < c.window.start - WEEK_MS || ts >= c.window.end) return;
    ClickTmp t;
    t.user = make_key(f[1].p, f[1].n);
    t.cid = make_key(f[0].p, f[0].n);
    t.h = hash_key(t.user);
    t.ts = ts;
    t.camp = camp;
    out.push_back(t);
}

inline Key person_key(const LinkDict& dict, const Key& user, u64 h) {
    u32 root = dict.lookup(user, h);
    return root != NONE32 ? Key{root, TAG_LINK} : user;
}

// Resolves clicks to persons (prefetching dictionary slots) and shuffles them.
inline void emit_clicks(ThreadData& td, const IngestContext& c, std::vector<ClickTmp>& tmp) {
    constexpr size_t PF = 12;
    for (size_t i = 0; i < tmp.size(); ++i) {
        if (i + PF < tmp.size()) __builtin_prefetch(c.dict->slot_address(tmp[i + PF].h));
        const ClickTmp& t = tmp[i];
        Key pk = person_key(*c.dict, t.user, t.h);
        C2* o = td.s2c[part_of(hash_key(pk), c.p2_bits)].alloc(td.pool);
        o->pk = pk;
        o->ts = t.ts;
        o->camp = t.camp;
        o->pad = 0;
        o->cid = t.cid;
        ++td.click_valid;
    }
    tmp.clear();
}

// Per-thread scratch for the dedup stage.
struct DedupScratch {
    std::vector<E1> rows;
    std::vector<u32> table, keep;
    std::vector<u64> hashes;
};

// Dedups one event-hash partition (earliest ingest wins) and forwards the
// valid survivors, keyed by person, to the person shuffle.
inline void dedup_partition(u32 part, std::vector<ThreadData>& all, ThreadData& me, DedupScratch& sc,
                            const IngestContext& c) {
    size_t n = 0;
    for (auto& t : all) n += t.s1[part].size();
    sc.rows.clear();
    sc.rows.reserve(n);
    for (auto& t : all) t.s1[part].drain_into(sc.rows, me.pool);
    if (n == 0) return;
    u32 cap = ceil_pow2(std::max<size_t>(n * 2, 16));
    sc.table.assign(cap, NONE32);
    const u32 mask = cap - 1;
    for (u32 i = 0; i < n; ++i) {
        const E1& r = sc.rows[i];
        size_t s = hash_key(r.ev) & mask;
        for (;;) {
            u32 t = sc.table[s];
            if (t == NONE32) {
                sc.table[s] = i;
                break;
            }
            if (sc.rows[t].ev == r.ev) {
                if (r.ingest < sc.rows[t].ingest) sc.table[s] = i;
                break;
            }
            s = (s + 1) & mask;
        }
    }
    sc.keep.clear();
    for (u32 t : sc.table)
        if (t != NONE32) {
            ++me.events_unique;
            if (sc.rows[t].pay & PAY_VALID) sc.keep.push_back(t);
        }
    me.events_valid += sc.keep.size();
    sc.hashes.resize(sc.keep.size());
    for (size_t j = 0; j < sc.keep.size(); ++j) sc.hashes[j] = hash_key(sc.rows[sc.keep[j]].user);
    constexpr size_t PF = 12;
    for (size_t j = 0; j < sc.keep.size(); ++j) {
        if (j + PF < sc.keep.size()) __builtin_prefetch(c.dict->slot_address(sc.hashes[j + PF]));
        const E1& r = sc.rows[sc.keep[j]];
        Key pk = person_key(*c.dict, r.user, sc.hashes[j]);
        E2* o = me.s2e[part_of(hash_key(pk), c.p2_bits)].alloc(me.pool);
        o->pk = pk;
        o->ts = r.ts;
        o->pay = r.pay;
    }
}
