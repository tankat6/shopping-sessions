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

inline void parse_event_line(const char* p, size_t len, const Cols<7>& cols, ThreadData& td,
                             const IngestContext& c) {
    Field f[7];
    PROBE_BEGIN(a0);
#ifdef ABL_NOFIELDS
    td.event_lines += cols.comma[3] & 1; return;
#endif
    bool split_ok = fields_from_cols<7>(p, len, cols, f);
    PROBE_END(a0, 10);
    if (!split_ok || f[0].n == 0) {
        ++td.bad_lines;
        return;
    }
    ++td.event_lines;
    E1 r;
    PROBE_BEGIN(a1);
    r.ev = make_key(f[0].p, f[0].n);
    i64 ts = NO_TIME, ing;
#ifdef ABL_NOTS
    bool ts_ok = true; ts = f[2].n; ing = f[3].n;
#else
    bool ts_ok = parse_ts(f[2].p, f[2].n, ts) && ts >= c.window.start && ts < c.window.end;
    if (!parse_ts(f[3].p, f[3].n, ing)) ing = NO_TIME;
#endif
    PROBE_END(a1, 11);
    PROBE_BEGIN(a2);
    u32 kind = 0, price = 0;
    {
        // Event type without branches: the type is one of three words in random order.
        const Field& ty = f[4];
        u32 w0, w1;
        std::memcpy(&w0, ty.p, 4);
        std::memcpy(&w1, ty.p + 4, 4);
        constexpr u32 VIEW = 'v' | 'i' << 8 | 'e' << 16 | (u32)'w' << 24;
        constexpr u32 CART = 'c' | 'a' << 8 | 'r' << 16 | (u32)'t' << 24;
        constexpr u32 PURC = 'p' | 'u' << 8 | 'r' << 16 | (u32)'c' << 24;
        constexpr u32 HASE = 'h' | 'a' << 8 | 's' << 16 | (u32)'e' << 24;
        const u32 is_view = (ty.n == 4) & (w0 == VIEW);
        const u32 is_cart = (ty.n == 4) & (w0 == CART);
        const u32 is_buy = (ty.n == 8) & (w0 == PURC) & (w1 == HASE);
        kind = is_view + 2 * is_cart + 3 * is_buy;
        if (is_buy && !parse_price(f[6].p, f[6].n, price)) kind = 0;
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
    PROBE_END(a2, 12);
    PROBE_BEGIN(a3);
#ifdef ABL_NOSHUF
    td.event_lines += (r.ev.a ^ r.ts ^ r.pay ^ r.user.a) & 1;
#else
    *td.s1[part_of(hash_key(r.ev), c.p1_bits)].alloc(td.pool) = r;
#endif
    PROBE_END(a3, 13);
}

inline void parse_link_line(const char* p, size_t len, const Cols<2>& cols, ThreadData& td) {
    Field f[2];
    if (!fields_from_cols<2>(p, len, cols, f) || f[0].n == 0 || f[1].n == 0) {
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

inline void parse_click_line(const char* p, size_t len, const Cols<4>& cols, ThreadData& td,
                             const IngestContext& c, std::vector<ClickTmp>& out) {
    Field f[4];
    PROBE_BEGIN(b0);
    bool split_ok = fields_from_cols<4>(p, len, cols, f);
    if (!split_ok || f[0].n == 0) {
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
    PROBE_END(b0, 14);
}

inline Key person_key(const LinkDict& dict, const Key& user, u64 h) {
    u32 root = dict.lookup(user, h);
    return root != NONE32 ? Key{root, TAG_LINK} : user;
}

// Resolves clicks to persons (prefetching dictionary slots) and shuffles them.
inline void emit_clicks(ThreadData& td, const IngestContext& c, std::vector<ClickTmp>& tmp) {
    PROBE_BEGIN(b1);
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
    PROBE_END(b1, 15);
}

// Per-thread scratch for the dedup stage.
struct KI {
    u64 a, b;
    i64 ingest;
};
struct DedupScratch {
    std::vector<KI> ki;
    std::vector<u32> table;
    std::vector<u8> win;
};

// Dedups one event-hash partition (earliest ingest wins) and forwards the
// valid survivors, keyed by person, to the person shuffle. Rows are read in
// place from the shuffle blocks; no partition-sized copy is made.
inline void dedup_partition(u32 part, std::vector<ThreadData>& all, ThreadData& me, DedupScratch& sc,
                            const IngestContext& c) {
    size_t n = 0;
    for (auto& t : all) n += t.s1[part].size();
    if (n == 0) return;
    PROBE_BEGIN(p0);
    sc.ki.resize(n);
    sc.win.assign(n, 0);
    u8* win = sc.win.data();
    u32 cap = ceil_pow2(std::max<size_t>(n * 2, 16));
    sc.table.assign(cap, NONE32);
    const u32 mask = cap - 1;
    u32* table = sc.table.data();
    KI* ki = sc.ki.data();
    size_t i = 0;
    for (auto& t : all)
        t.s1[part].for_each_block([&](const E1* b, size_t cnt) {
            for (size_t j = 0; j < cnt; ++j, ++i) {
                const E1& r = b[j];
                ki[i] = KI{r.ev.a, r.ev.b, r.ingest};
                size_t s = hash_key(r.ev) & mask;
                for (;;) {
                    u32 t0 = table[s];
                    if (t0 == NONE32) {
                        table[s] = (u32)i;
                        win[i] = 1;
                        ++me.events_unique;
                        break;
                    }
                    if (ki[t0].a == r.ev.a && ki[t0].b == r.ev.b) {
                        if (r.ingest < ki[t0].ingest) {
                            table[s] = (u32)i;
                            win[t0] = 0;
                            win[i] = 1;
                        }
                        break;
                    }
                    s = (s + 1) & mask;
                }
            }
        });
    PROBE_END(p0, 0);
    PROBE_BEGIN(p2);
    // Forward valid winners; dictionary slots are prefetched a few rows ahead.
    constexpr size_t PF = 16;
    struct Pending {
        const E1* r;
        u64 h;
    } ring[PF];
    size_t k = 0;
    auto emit = [&](const Pending& p) {
        Key pk = person_key(*c.dict, p.r->user, p.h);
        E2* o = me.s2e[part_of(hash_key(pk), c.p2_bits)].alloc(me.pool);
        o->pk = pk;
        o->ts = p.r->ts;
        o->pay = p.r->pay;
    };
    i = 0;
    for (auto& t : all)
        t.s1[part].for_each_block([&](const E1* b, size_t cnt) {
            for (size_t j = 0; j < cnt; ++j, ++i) {
                if (!win[i] || !(b[j].pay & PAY_VALID)) continue;
                u64 h = hash_key(b[j].user);
                __builtin_prefetch(c.dict->slot_address(h));
                if (k >= PF) emit(ring[k % PF]);
                ring[k % PF] = Pending{&b[j], h};
                ++k;
            }
        });
    for (size_t m = k > PF ? k - PF : 0; m < k; ++m) emit(ring[m % PF]);
    me.events_valid += k;
    for (auto& t : all) t.s1[part].release(me.pool);
    PROBE_END(p2, 2);
}
