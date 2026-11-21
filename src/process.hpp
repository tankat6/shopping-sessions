// Per-person processing: sessions, attribution, assists and aggregation.
#pragma once
#include <unordered_map>

#include "ingest.hpp"
#include "parquet.hpp"
#include "radix.hpp"

struct CampCell {
    u64 revenue = 0;
    u32 credited = 0, purchasing = 0, assisted = 0;
};

// Campaign-day accumulator: dense array when small, hash map otherwise.
class CampaignAcc {
public:
    void init(u32 camps, i64 base_day, u32 ndays) {
        base_ = base_day;
        ndays_ = ndays;
        dense_ok_ = (u64)camps * ndays <= (1u << 24);
        if (dense_ok_) dense_.assign((size_t)camps * ndays, CampCell{});
    }
    CampCell& at(u32 camp, i64 day) {
        i64 off = day - base_;
        if (dense_ok_ && off >= 0 && off < (i64)ndays_) return dense_[(size_t)camp * ndays_ + (size_t)off];
        return sparse_[((u64)camp << 32) | (u32)(off + 0x40000000)];
    }
    // fn(camp, day, cell) for every cell with sessions or assists.
    template <class F>
    void for_each(F&& fn) const {
        if (dense_ok_)
            for (size_t i = 0; i < dense_.size(); ++i) {
                const CampCell& c = dense_[i];
                if (c.credited || c.assisted) fn((u32)(i / ndays_), base_ + (i64)(i % ndays_), c);
            }
        for (const auto& kv : sparse_)
            fn((u32)(kv.first >> 32), (i64)(u32)kv.first - 0x40000000 + base_, kv.second);
    }
    void merge(const CampaignAcc& o) {
        for (size_t i = 0; i < o.dense_.size(); ++i) add(dense_[i], o.dense_[i]);
        for (const auto& kv : o.sparse_) add(sparse_[kv.first], kv.second);
    }

private:
    static void add(CampCell& a, const CampCell& b) {
        a.revenue += b.revenue;
        a.credited += b.credited;
        a.purchasing += b.purchasing;
        a.assisted += b.assisted;
    }
    i64 base_ = 0;
    u32 ndays_ = 1;
    bool dense_ok_ = true;
    std::vector<CampCell> dense_;
    std::unordered_map<u64, CampCell> sparse_;
};

struct ProcessContext {
    const Products* products;
    const Campaigns* campaigns;
    const LinkDict* dict;
    i64 base_day;
    u32 ndays;
    u32 p2_bits;
    i64 win_start;     // event timestamps are stored relative to this
    u32 tbits;         // bits needed for an event timestamp offset
    u32 ctbits;        // bits needed for a click timestamp offset (window start - 7 days)
};

class PersonWorker {
public:
    PersonWorker(const ProcessContext& c, std::string user_path, bool create_empty)
        : ctx_(c),
          out_(std::move(user_path), {{"user_id", pq::BYTE_ARRAY, false, pq::STRING},
                                      {"day", pq::INT32, false, pq::DATE},
                                      {"sessions", pq::INT64, false, pq::PLAIN_TYPE},
                                      {"events", pq::INT64, false, pq::PLAIN_TYPE},
                                      {"revenue_cents", pq::INT64, false, pq::PLAIN_TYPE},
                                      {"top_category", pq::BYTE_ARRAY, true, pq::STRING}}),
          create_empty_(create_empty) {
        const u32 camps = c.campaigns->organic() + 1;
        acc.init(camps, c.base_day, c.ndays);
        covered_.assign(camps, 0);
        cat_sum_.assign(c.products->category_name.size(), 0);
    }

    CampaignAcc acc;
    u64 persons = 0, sessions = 0, user_days = 0;

    void process_partition(u32 part, std::vector<ThreadData>& all, ThreadData& me);
    void finish() { out_.finish(create_empty_); }

private:
    struct Sess {
        i64 start, revenue;
        u32 events, group;
        bool purchasing;
    };
    struct Group {
        i64 day;
        u64 revenue;
        u32 first, end, sessions, events, top;
    };

    const ProcessContext& ctx_;
    pq::File out_;
    bool create_empty_;
    struct Click {
        i64 ts;
        u32 camp;
    };
    std::vector<C2> cv_;
    std::vector<KRec> recs_, tmp_, crecs_, ctmp_;
    std::vector<Key> pkeys_;
    std::vector<Click> cl_;
    std::vector<u32> table_, covered_, touched_camps_, touched_cats_;
    std::vector<u64> cat_sum_;
    std::vector<Sess> sess_;
    std::vector<Group> groups_;

    void close_group() {
        if (groups_.empty()) return;
        Group& g = groups_.back();
        g.end = (u32)sess_.size();
        u32 best = NONE32;
        u64 best_sum = 0;
        const auto& rank = ctx_.products->category_rank;
        for (u32 c : touched_cats_) {
            u64 s = cat_sum_[c];
            if (best == NONE32 || s > best_sum || (s == best_sum && rank[c] < rank[best])) {
                best = c;
                best_sum = s;
            }
            cat_sum_[c] = 0;
        }
        touched_cats_.clear();
        g.top = best;
    }
    void add_assist(u32 camp, u32 l, u32 r) {
        u32 g = sess_[l].group;
        while (g < groups_.size() && groups_[g].first < r) {
            u32 gl = std::max(l, groups_[g].first), gr = std::min(r, groups_[g].end);
            acc.at(camp, groups_[g].day).assisted += gr - gl;
            ++g;
        }
    }
    void process_person(const Key& pk, const KRec* e, size_t n, KRec* c, size_t m);
    void emit_group(const Key& name, const Group& g);
};

inline void PersonWorker::emit_group(const Key& name, const Group& g) {
    char tmp[16];
    std::string_view nm = key_view(name, tmp);
    out_.add_str(0, nm.data(), nm.size());
    out_.add_i32(1, g.day);
    out_.add_i64(2, g.sessions);
    out_.add_i64(3, g.events);
    out_.add_i64(4, (i64)g.revenue);
    if (g.top != NONE32) {
        const std::string& cn = ctx_.products->category_name[g.top];
        out_.add_str(5, cn.data(), cn.size());
    } else {
        out_.add_null(5);
    }
    out_.row_done();
    ++user_days;
}

inline void PersonWorker::process_person(const Key& pk, const KRec* e, size_t n, KRec* c, size_t m) {
    PROBE_BEGIN(r0);
    const u64 tmask = ctx_.tbits >= 64 ? ~0ull : ((1ull << ctx_.tbits) - 1);
    sess_.clear();
    groups_.clear();
    i64 prev = 0;
    for (size_t i = 0; i < n; ++i) {
        const i64 ts = (i64)(e[i].key & tmask) + ctx_.win_start;
        if (i == 0 || ts - prev > SESSION_GAP_MS) {
            i64 day = floor_div(ts, DAY_MS);
            if (groups_.empty() || groups_.back().day != day) {
                close_group();
                groups_.push_back(Group{day, 0, (u32)sess_.size(), 0, 0, 0, NONE32});
            }
            sess_.push_back(Sess{ts, 0, 0, (u32)groups_.size() - 1, false});
            ++groups_.back().sessions;
        }
        prev = ts;
        Sess& s = sess_.back();
        Group& g = groups_.back();
        ++s.events;
        ++g.events;
        const u64 pay = e[i].val;
        if (pay_kind(pay) == 3) {
            u32 price = pay_price(pay), cat = pay_category(pay);
            s.revenue += price;
            g.revenue += price;
            s.purchasing = true;
            if (cat_sum_[cat] == 0) touched_cats_.push_back(cat);
            cat_sum_[cat] += price;
        }
    }
    close_group();
    PROBE_END(r0, 6);
    ++persons;
    sessions += sess_.size();

    PROBE_BEGIN(r1);
    // Clicks arrive sorted by timestamp. Equal timestamps are ordered by
    // click_id descending (so the smallest id comes last) and repeated IDs
    // are dropped.
    cl_.clear();
    {
        const u64 cmask = ctx_.ctbits >= 64 ? ~0ull : ((1ull << ctx_.ctbits) - 1);
        const i64 cbase = ctx_.win_start - WEEK_MS;
        for (size_t i = 0; i < m;) {
            size_t j = i + 1;
            while (j < m && c[j].key == c[i].key) ++j;
            if (j - i > 1)
                std::sort(c + i, c + j, [&](const KRec& x, const KRec& y) {
                    return key_cmp(cv_[(u32)x.val].cid, cv_[(u32)y.val].cid) > 0;
                });
            for (size_t q = i; q < j; ++q) {
                if (q > i && cv_[(u32)c[q].val].cid == cv_[(u32)c[q - 1].val].cid) continue;
                cl_.push_back(Click{(i64)(c[q].key & cmask) + cbase, (u32)(c[q].val >> 32)});
            }
            i = j;
        }
    }
    const Click* ck = cl_.data();
    const size_t k = cl_.size();

    const u32 organic = ctx_.campaigns->organic();
    const u32 ns = (u32)sess_.size();
    size_t add = 0, rem = 0;
    for (u32 si = 0; si < ns; ++si) {
        const Sess& s = sess_[si];
        while (add < k && ck[add].ts <= s.start) ++add;
        while (rem < add && ck[rem].ts < s.start - WEEK_MS) ++rem;
        u32 camp = add > rem ? ck[add - 1].camp : organic;
        CampCell& cell = acc.at(camp, groups_[s.group].day);
        ++cell.credited;
        cell.purchasing += s.purchasing;
        cell.revenue += (u64)s.revenue;
    }

    // Assists: each click covers sessions starting in [ts, ts + 7 days].
    // Ranges of one campaign are unioned through covered_ (both ends are
    // monotone in click order).
    u32 lo = 0, hi = 0;
    for (size_t j = 0; j < k; ++j) {
        while (lo < ns && sess_[lo].start < ck[j].ts) ++lo;
        while (hi < ns && sess_[hi].start <= ck[j].ts + WEEK_MS) ++hi;
        u32 camp = ck[j].camp;
        u32 l = std::max(lo, covered_[camp]);
        if (hi > l) {
            if (covered_[camp] == 0) touched_camps_.push_back(camp);
            add_assist(camp, l, hi);
            covered_[camp] = hi;
        }
    }
    for (u32 camp : touched_camps_) covered_[camp] = 0;
    touched_camps_.clear();

    PROBE_END(r1, 7);
    PROBE_BEGIN(r2);
    Key name = (pk.b >> 56) == 0xFE ? ctx_.dict->name_of((u32)pk.a) : pk;
    for (const Group& g : groups_) emit_group(name, g);
    PROBE_END(r2, 8);
}

inline void PersonWorker::process_partition(u32 part, std::vector<ThreadData>& all, ThreadData& me) {
    PROBE_BEGIN(q0);
    size_t ne = 0, nc = 0;
    for (auto& t : all) {
        ne += t.s2e[part].size();
        nc += t.s2c[part].size();
    }
    cv_.clear();
    cv_.reserve(nc);
    for (auto& t : all) t.s2c[part].drain_into(cv_, me.pool);
    if (ne == 0) {
        for (auto& t : all) t.s2e[part].release(me.pool);
        return;
    }

    // Number the persons of this partition and write one sort record per event.
    u32 cap = ceil_pow2(std::max<size_t>(ne * 2, 16));
    table_.assign(cap, NONE32);
    const u32 mask = cap - 1;
    u32* table = table_.data();
    pkeys_.clear();
    recs_.resize(ne);
    tmp_.resize(ne);
    KRec* out = recs_.data();
    const u32 tb = ctx_.tbits;
    const i64 ws = ctx_.win_start;
    for (auto& t : all) {
        t.s2e[part].for_each_block([&](const E2* b, size_t cnt) {
            for (size_t j = 0; j < cnt; ++j) {
                const Key& pk = b[j].pk;
                size_t s = hash_key(pk) & mask;
                u32 idx;
                for (;;) {
                    u32 t0 = table[s];
                    if (t0 == NONE32) {
                        idx = (u32)pkeys_.size();
                        table[s] = idx;
                        pkeys_.push_back(pk);
                        break;
                    }
                    if (pkeys_[t0] == pk) {
                        idx = t0;
                        break;
                    }
                    s = (s + 1) & mask;
                }
                *out++ = KRec{((u64)idx << tb) | (u64)(b[j].ts - ws), b[j].pay};
            }
        });
        t.s2e[part].release(me.pool);
    }
    const u32 np = (u32)pkeys_.size();
    const u32 idxbits = np > 1 ? 32 - (u32)__builtin_clz(np - 1) : 0;
    if (idxbits + tb > 64 || idxbits + ctx_.ctbits > 64) throw std::runtime_error("window too long for the sort key");

    // Clicks of persons that have events.
    crecs_.clear();
    const u32 ctb = ctx_.ctbits;
    const i64 cbase = ws - WEEK_MS;
    for (size_t j = 0; j < cv_.size(); ++j) {
        const Key& pk = cv_[j].pk;
        size_t s = hash_key(pk) & mask;
        for (;;) {
            u32 t0 = table[s];
            if (t0 == NONE32) break;
            if (pkeys_[t0] == pk) {
                crecs_.push_back(KRec{((u64)t0 << ctb) | (u64)(cv_[j].ts - cbase), (u64)j | ((u64)cv_[j].camp << 32)});
                break;
            }
            s = (s + 1) & mask;
        }
    }
    ctmp_.resize(crecs_.size());
    const KRec* E = radix_sort(recs_.data(), tmp_.data(), ne, tb + idxbits);
    KRec* C = radix_sort(crecs_.data(), ctmp_.data(), crecs_.size(), ctb + idxbits);
    const size_t nc2 = crecs_.size();
    PROBE_END(q0, 3);
    PROBE_BEGIN(q2);

    constexpr u32 AHEAD = 8;
    size_t ei = 0, ci = 0;
    for (u32 p = 0; p < np; ++p) {
        if (p + AHEAD < np) {
            const Key& k2 = pkeys_[p + AHEAD];
            if ((k2.b >> 56) == 0xFE) __builtin_prefetch(ctx_.dict->name_address((u32)k2.a));
        }
        size_t ee = ei;
        while (ee < ne && (E[ee].key >> tb) == p) ++ee;
        size_t cc = ci;
        while (cc < nc2 && (C[cc].key >> ctb) == p) ++cc;
        process_person(pkeys_[p], E + ei, ee - ei, C + ci, cc - ci);
        ei = ee;
        ci = cc;
    }
    PROBE_END(q2, 5);
}
