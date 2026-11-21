// Per-person processing: sessions, attribution, assists and aggregation.
#pragma once
#include <unordered_map>

#include "ingest.hpp"
#include "parquet.hpp"

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
    std::vector<E2> ev_, evs_;
    std::vector<C2> cv_, cvs_;
    std::vector<u32> table_, first_, pe_, pc_, off_e_, off_c_, covered_, touched_camps_, touched_cats_;
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
    void process_person(const Key& pk, E2* e, size_t n, C2* c, size_t m);
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

inline void PersonWorker::process_person(const Key& pk, E2* e, size_t n, C2* c, size_t m) {
    std::sort(e, e + n, [](const E2& x, const E2& y) { return x.ts < y.ts; });
    sess_.clear();
    groups_.clear();
    i64 prev = 0;
    for (size_t i = 0; i < n; ++i) {
        const i64 ts = e[i].ts;
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
        if (pay_kind(e[i].pay) == 3) {
            u32 price = pay_price(e[i].pay), cat = pay_category(e[i].pay);
            s.revenue += price;
            g.revenue += price;
            s.purchasing = true;
            if (cat_sum_[cat] == 0) touched_cats_.push_back(cat);
            cat_sum_[cat] += price;
        }
    }
    close_group();
    ++persons;
    sessions += sess_.size();

    // Clicks: latest first by (ts, smallest id); drop repeated click IDs.
    size_t k = 0;
    if (m) {
        std::sort(c, c + m, [](const C2& x, const C2& y) {
            if (x.ts != y.ts) return x.ts < y.ts;
            return key_cmp(x.cid, y.cid) > 0;
        });
        for (size_t i = 0; i < m; ++i) {
            if (k && c[k - 1].ts == c[i].ts && c[k - 1].cid == c[i].cid) continue;
            c[k++] = c[i];
        }
    }

    const u32 organic = ctx_.campaigns->organic();
    const u32 ns = (u32)sess_.size();
    size_t add = 0, rem = 0;
    for (u32 si = 0; si < ns; ++si) {
        const Sess& s = sess_[si];
        while (add < k && c[add].ts <= s.start) ++add;
        while (rem < add && c[rem].ts < s.start - WEEK_MS) ++rem;
        u32 camp = add > rem ? c[add - 1].camp : organic;
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
        while (lo < ns && sess_[lo].start < c[j].ts) ++lo;
        while (hi < ns && sess_[hi].start <= c[j].ts + WEEK_MS) ++hi;
        u32 camp = c[j].camp;
        u32 l = std::max(lo, covered_[camp]);
        if (hi > l) {
            if (covered_[camp] == 0) touched_camps_.push_back(camp);
            add_assist(camp, l, hi);
            covered_[camp] = hi;
        }
    }
    for (u32 camp : touched_camps_) covered_[camp] = 0;
    touched_camps_.clear();

    Key name = (pk.b >> 56) == 0xFE ? ctx_.dict->name_of((u32)pk.a) : pk;
    for (const Group& g : groups_) emit_group(name, g);
}

inline void PersonWorker::process_partition(u32 part, std::vector<ThreadData>& all, ThreadData& me) {
    size_t ne = 0, nc = 0;
    for (auto& t : all) {
        ne += t.s2e[part].size();
        nc += t.s2c[part].size();
    }
    ev_.clear();
    cv_.clear();
    ev_.reserve(ne);
    cv_.reserve(nc);
    for (auto& t : all) {
        t.s2e[part].drain_into(ev_, me.pool);
        t.s2c[part].drain_into(cv_, me.pool);
    }
    if (ne == 0) return;

    // Dense local person numbers.
    u32 cap = ceil_pow2(std::max<size_t>(ne * 2, 16));
    table_.assign(cap, NONE32);
    const u32 mask = cap - 1;
    first_.clear();
    pe_.resize(ne);
    for (size_t i = 0; i < ne; ++i) {
        const Key& pk = ev_[i].pk;
        size_t s = hash_key(pk) & mask;
        for (;;) {
            u32 t = table_[s];
            if (t == NONE32) {
                table_[s] = (u32)first_.size();
                pe_[i] = (u32)first_.size();
                first_.push_back((u32)i);
                break;
            }
            if (ev_[first_[t]].pk == pk) {
                pe_[i] = t;
                break;
            }
            s = (s + 1) & mask;
        }
    }
    const u32 np = (u32)first_.size();
    off_e_.assign(np + 1, 0);
    for (size_t i = 0; i < ne; ++i) ++off_e_[pe_[i] + 1];
    for (u32 p = 0; p < np; ++p) off_e_[p + 1] += off_e_[p];
    evs_.resize(ne);
    {
        std::vector<u32> pos(off_e_.begin(), off_e_.end() - 1);
        for (size_t i = 0; i < ne; ++i) evs_[pos[pe_[i]]++] = ev_[i];
    }

    pc_.resize(nc);
    off_c_.assign(np + 1, 0);
    for (size_t j = 0; j < nc; ++j) {
        const Key& pk = cv_[j].pk;
        size_t s = hash_key(pk) & mask;
        u32 found = NONE32;
        for (;;) {
            u32 t = table_[s];
            if (t == NONE32) break;
            if (ev_[first_[t]].pk == pk) {
                found = t;
                break;
            }
            s = (s + 1) & mask;
        }
        pc_[j] = found;
        if (found != NONE32) ++off_c_[found + 1];
    }
    for (u32 p = 0; p < np; ++p) off_c_[p + 1] += off_c_[p];
    cvs_.resize(off_c_[np]);
    {
        std::vector<u32> pos(off_c_.begin(), off_c_.end() - 1);
        for (size_t j = 0; j < nc; ++j)
            if (pc_[j] != NONE32) cvs_[pos[pc_[j]]++] = cv_[j];
    }

    for (u32 p = 0; p < np; ++p)
        process_person(ev_[first_[p]].pk, evs_.data() + off_e_[p], off_e_[p + 1] - off_e_[p], cvs_.data() + off_c_[p],
                       off_c_[p + 1] - off_c_[p]);
}
