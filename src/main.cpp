// Shopping Sessions: native pipeline. Reads CSV, writes Parquet.
//
// Stages: reference + links -> parse/shuffle by event id -> dedup and person
// resolution -> shuffle by person -> sessions/attribution/aggregation.
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>

#include "process.hpp"

static std::string env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return v && *v ? v : fallback;
}

static void make_dirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); ++i)
        if (i == path.size() || path[i] == '/') {
            std::string p = path.substr(0, i);
            if (::mkdir(p.c_str(), 0777) != 0 && errno != EEXIST) throw std::runtime_error("cannot create " + p);
        }
}

// Removes part-*.parquet files left by an earlier run so they cannot mix in.
static void clear_parts(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.rfind("part-", 0) == 0 && n.size() > 8 && n.compare(n.size() - 8, 8, ".parquet") == 0)
            ::unlink((dir + "/" + n).c_str());
    }
    closedir(d);
}

static u32 partition_bits(u64 est_rows) {
    u32 p = ceil_pow2(std::max<u64>(est_rows / 50000, 1));
    return log2_exact(std::clamp<u32>(p, 64, 4096));
}

int main(int argc, char** argv) {
    try {
#ifdef __AVX2__
        if (!__builtin_cpu_supports("avx2")) throw std::runtime_error("this build requires AVX2");
#endif
        std::string data = env_or("DATA_DIR", "/data"), out = env_or("OUTPUT_DIR", "/output");
        if (argc >= 3) {
            data = argv[1];
            out = argv[2];
        }
        const int T = detect_threads();
        Profile prof;
        prof.verbose = !std::getenv("QUIET");
        std::fprintf(stderr, "threads: %d\n", T);

        // --- references and links
        Window win = load_manifest(data + "/manifest.json");
        Products products;
        products.load(data + "/products.csv");
        Campaigns campaigns;
        campaigns.load(data + "/campaigns.csv");
        prof.mark("refs");
        make_dirs(out + "/user_daily");
        make_dirs(out + "/campaign_daily");
        clear_parts(out + "/user_daily");
        clear_parts(out + "/campaign_daily");

        auto link_files = list_csv(data + "/links");
        auto event_files = list_csv(data + "/events");
        auto click_files = list_csv(data + "/clicks");
        u64 CHUNK = 4u << 20;
        if (const char* e = std::getenv("CHUNK_BYTES")) CHUNK = std::max<u64>(1, std::strtoull(e, nullptr, 10));
        std::vector<ThreadData> td(T);
        {
            auto chunks = make_chunks(link_files, CHUNK);
            std::vector<ChunkReader> readers(T);
            parallel_jobs(T, chunks.size(), [&](int t, size_t j) {
                readers[t].for_each_line(link_files[chunks[j].file], chunks[j], [&](const char* p, size_t n) { parse_link_line(p, n, td[t]); });
            });
        }
        prof.mark("link_parse");
        std::vector<std::vector<LinkPair>> links(T);
        u64 link_count = 0;
        for (int t = 0; t < T; ++t) {
            links[t] = std::move(td[t].links);
            link_count += links[t].size();
        }
        LinkDict dict;
        dict.build(links, T);
        prof.count("links", link_count);
        prof.count("link_ids", dict.ids());
        prof.mark("link_dict");

        // --- parse events and clicks
        u64 event_bytes = 0;
        for (auto& f : event_files) event_bytes += f.size;
        u32 p1_bits = partition_bits(event_bytes / 80);
        if (const char* e = std::getenv("PART_BITS")) p1_bits = (u32)std::clamp(std::atoi(e), 0, 12);
        const u32 p2_bits = p1_bits;
        const u32 P1 = 1u << p1_bits, P2 = 1u << p2_bits;
        for (auto& t : td) {
            t.s1.resize(P1);
            t.s2e.resize(P2);
            t.s2c.resize(P2);
        }
        IngestContext ic{&products, &campaigns, &dict, win, p1_bits, p2_bits};
        {
            auto ec = make_chunks(event_files, CHUNK);
            auto cc = make_chunks(click_files, CHUNK);
            struct Job {
                bool click;
                size_t idx;
            };
            std::vector<Job> jobs;
            for (size_t i = 0; i < ec.size(); ++i) jobs.push_back({false, i});
            for (size_t i = 0; i < cc.size(); ++i) jobs.push_back({true, i});
            std::vector<ChunkReader> readers(T);
            std::vector<std::vector<ClickTmp>> tmp(T);
            parallel_jobs(T, jobs.size(), [&](int t, size_t j) {
                const Job& job = jobs[j];
                if (!job.click) {
                    readers[t].for_each_line(event_files[ec[job.idx].file], ec[job.idx],
                                             [&](const char* p, size_t n) { parse_event_line(p, n, td[t], ic); });
                } else {
                    readers[t].for_each_line(click_files[cc[job.idx].file], cc[job.idx],
                                             [&](const char* p, size_t n) { parse_click_line(p, n, td[t], ic, tmp[t]); });
                    emit_clicks(td[t], ic, tmp[t]);
                }
            });
        }
        u64 ev_lines = 0, ev_bad = 0, cl_lines = 0, cl_bad = 0;
        for (auto& t : td) {
            ev_lines += t.event_lines;
            ev_bad += t.bad_lines;
            cl_lines += t.click_lines;
            cl_bad += t.click_bad;
        }
        prof.count("event_lines", ev_lines);
        prof.count("event_malformed", ev_bad);
        prof.count("click_lines", cl_lines);
        prof.count("click_malformed", cl_bad);
        prof.mark("parse");

        // --- dedup events by id and resolve persons
        {
            std::vector<DedupScratch> scratch(T);
            parallel_jobs(T, P1, [&](int t, size_t part) { dedup_partition((u32)part, td, td[t], scratch[t], ic); });
        }
        u64 uniq = 0, valid = 0, clicks = 0;
        for (auto& t : td) {
            uniq += t.events_unique;
            valid += t.events_valid;
            clicks += t.click_valid;
        }
        prof.count("events_unique", uniq);
        prof.count("events_valid", valid);
        prof.count("click_rows_valid", clicks);
        prof.mark("dedup");

        // --- sessions, attribution, aggregation, user_daily output
        const i64 base_day = floor_div(win.start, DAY_MS);
        const u32 ndays = (u32)(floor_div(win.end - 1, DAY_MS) - base_day + 1);
        ProcessContext pc{&products, &campaigns, &dict, base_day, ndays, p2_bits};
        std::vector<std::unique_ptr<PersonWorker>> workers;
        for (int t = 0; t < T; ++t)
            workers.emplace_back(new PersonWorker(pc, out + "/user_daily/part-" + std::to_string(t) + ".parquet", t == 0));
        parallel_jobs(T, P2, [&](int t, size_t part) { workers[t]->process_partition((u32)part, td, td[t]); });
        run_threads(T, [&](int t) { workers[t]->finish(); });
        u64 persons = 0, sessions = 0, user_days = 0;
        for (auto& w : workers) {
            persons += w->persons;
            sessions += w->sessions;
            user_days += w->user_days;
        }
        prof.count("persons", persons);
        prof.count("sessions", sessions);
        prof.count("user_days", user_days);
        prof.mark("sessions+user_daily");

        // --- campaign_daily
        for (int t = 1; t < T; ++t) workers[0]->acc.merge(workers[t]->acc);
        pq::File cf(out + "/campaign_daily/part-0.parquet",
                    {{"campaign_id", pq::BYTE_ARRAY, false, pq::STRING},
                     {"channel", pq::BYTE_ARRAY, false, pq::STRING},
                     {"day", pq::INT32, false, pq::DATE},
                     {"sessions", pq::INT64, false, pq::PLAIN_TYPE},
                     {"purchasing_sessions", pq::INT64, false, pq::PLAIN_TYPE},
                     {"revenue_cents", pq::INT64, false, pq::PLAIN_TYPE},
                     {"assisted_sessions", pq::INT64, false, pq::PLAIN_TYPE}});
        const u32 organic = campaigns.organic();
        workers[0]->acc.for_each([&](u32 camp, i64 day, const CampCell& c) {
            const std::string org = "organic";
            const std::string& id = camp == organic ? org : campaigns.id[camp];
            const std::string& ch = camp == organic ? org : campaigns.channel[camp];
            cf.add_str(0, id.data(), id.size());
            cf.add_str(1, ch.data(), ch.size());
            cf.add_i32(2, day);
            cf.add_i64(3, c.credited);
            cf.add_i64(4, c.purchasing);
            cf.add_i64(5, (i64)c.revenue);
            cf.add_i64(6, camp == organic ? 0 : c.assisted);
            cf.row_done();
        });
        cf.finish(true);
        prof.mark("campaign_daily");
        if (prof.verbose) std::fprintf(stderr, "TOTAL %.3fs\n", prof.total.lap());
        // Outputs are closed; skip tearing down gigabytes of working memory.
        std::fflush(stderr);
        ::_exit(0);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}
