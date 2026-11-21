// Shopping Sessions: native pipeline. Reads CSV, writes Parquet.
//
// Stages: reference + links -> parse/shuffle by event id -> dedup and person
// resolution -> shuffle by person -> sessions/attribution/aggregation.
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <malloc.h>
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
        if (!__builtin_cpu_supports("avx2") || !__builtin_cpu_supports("bmi") || !__builtin_cpu_supports("bmi2") ||
            !__builtin_cpu_supports("popcnt"))
            throw std::runtime_error("this build requires AVX2, BMI1/2 and POPCNT");
#endif
        std::string data = env_or("DATA_DIR", "/data"), out = env_or("OUTPUT_DIR", "/output");
        if (argc >= 3) {
            data = argv[1];
            out = argv[2];
        }
        // Keep glibc from returning memory to the kernel or growing heaps in tiny steps:
        // each such call takes the address-space lock exclusively (MALLOPT=0 disables).
        if (!std::getenv("MALLOPT") || std::atoi(std::getenv("MALLOPT")) != 0) {
            mallopt(M_MMAP_THRESHOLD, 32 << 20);
            mallopt(M_TRIM_THRESHOLD, 1 << 30);
            mallopt(M_TOP_PAD, 64 << 20);
        }
        if (const char* e = std::getenv("THP")) g_huge_pages = std::atoi(e) != 0;
        pool_owner_slot();  // the main thread owns the persistent worker pool
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
        open_inputs(link_files);
        open_inputs(event_files);
        open_inputs(click_files);
        // Input access: pread into per-thread buffers by default; INPUT=mmap maps the files instead.
        u64 CHUNK = 4u << 20;
        if (const char* e = std::getenv("CHUNK_BYTES")) CHUNK = std::max<u64>(1, std::strtoull(e, nullptr, 10));
        // Diagnostic: how fast can this machine just read the input? IO_ONLY=1 preads every
        // chunk, IO_ONLY=scan also counts newlines, IO_ONLY=mmap touches a mapping. Exits afterwards.
        if (const char* io = std::getenv("IO_ONLY")) {
            std::vector<InputFile> all;
            for (auto* v : {&link_files, &event_files, &click_files})
                for (auto& f : *v) all.push_back(f);
            auto chunks = make_chunks(all, CHUNK);
            const std::string mode = io;
            std::atomic<u64> bytes{0}, lines{0};
            std::vector<MappedFile> maps;
            if (mode == "mmap")
                for (auto& f : all) maps.push_back(map_file(f));
            Stopwatch sw;
            parallel_jobs(T, chunks.size(), [&](int, size_t j) {
                const Chunk& c = chunks[j];
                const size_t n = (size_t)(c.end - c.begin);
                u64 cnt = 0;
                if (mode == "mmap") {
                    const char* p = maps[c.file].p;
                    if (!p) return;
                    ::madvise((void*)(p + (c.begin & ~4095ull)), (size_t)(c.end - (c.begin & ~4095ull)), MADV_POPULATE_READ);
                    for (u64 o = c.begin; o < c.end; o += 4096) cnt += (u8)p[o];
                } else {
                    thread_local std::vector<char> buf;
                    if (buf.size() < n) buf.resize(n);
                    size_t got = 0;
                    while (got < n) {
                        ssize_t r = ::pread(all[c.file].fd, buf.data() + got, n - got, (off_t)(c.begin + got));
                        if (r <= 0) break;
                        got += (size_t)r;
                    }
                    if (mode == "scan")
                        for (size_t o = 0; o < got;) {
                            const void* q = std::memchr(buf.data() + o, '\n', got - o);
                            if (!q) break;
                            ++cnt;
                            o = (size_t)((const char*)q - buf.data()) + 1;
                        }
                }
                bytes += n;
                lines += cnt;
            });
            double sec = sw.lap();
            std::fprintf(stderr, "IO_FLOOR mode=%s threads=%d bytes=%.0f MB time=%.3fs rate=%.0f MB/s (checksum %llu)\n",
                         io, T, (double)bytes / 1e6, sec, (double)bytes / 1e6 / sec, (unsigned long long)lines);
            ::_exit(0);
        }
        const char* input_mode = std::getenv("INPUT");
        const bool use_map = input_mode && std::strcmp(input_mode, "mmap") == 0;
        std::vector<ThreadData> td(T);
        {
            auto chunks = make_chunks(link_files, CHUNK);
            std::vector<MappedFile> maps;
            for (auto& f : link_files) maps.push_back(use_map ? map_file(f) : MappedFile{});
            std::vector<ChunkReader> readers(T);
            parallel_jobs(T, chunks.size(), [&](int t, size_t j) {
                readers[t].for_each_record<2>(link_files[chunks[j].file], maps[chunks[j].file], chunks[j],
                                              [&](const char* p, size_t n, const Cols<2>& c) { parse_link_line(p, n, c, td[t]); });
            });
            for (int t = 0; t < T; ++t) thread_stats().read_ns[t].fetch_add(readers[t].read_ns);
        }
        prof.mark("link_parse");
        std::vector<std::vector<LinkPair>> links(T);
        u64 link_count = 0;
        for (int t = 0; t < T; ++t) {
            links[t] = std::move(td[t].links);
            link_count += links[t].size();
        }
        // The identity dictionary is only needed once clicks are resolved, so it is
        // built as one job beside the event chunks. It runs on a single worker (no
        // extra threads, so never more runnable threads than CPUs) and the other
        // workers keep parsing; dynamic scheduling balances the rest.
        LinkDict dict;
        std::atomic<int> dict_state{0};  // 0 pending, 1 built, 2 failed
        prof.count("links", link_count);

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
        // One reservation for all shuffle blocks (ARENA=0 falls back to per-thread slabs).
        if (!std::getenv("ARENA") || std::atoi(std::getenv("ARENA")) != 0) {
            u64 click_bytes = 0;
            for (auto& f : click_files) click_bytes += f.size;
            // Upper bound: shortest plausible rows, plus slack for block rounding and stride padding.
            u64 need = (event_bytes / 48) * sizeof(E1) + (click_bytes / 24) * sizeof(C2);
            need += need / 8 + ((u64)T << 24) + ((u64)P1 * 3 * T << 15);
            need += link_count * 288 + (4ull << 20);  // dictionary tables: at most 8 slots of 32 bytes per link
            if (!SlabSource::get().reserve((size_t)need)) std::fprintf(stderr, "note: arena reservation refused, using slabs\n");
        }
        IngestContext ic{&products, &campaigns, &dict, win, p1_bits, p2_bits};
        {
            auto ec = make_chunks(event_files, CHUNK);
            auto cc = make_chunks(click_files, CHUNK);
            struct Job {
                bool click;
                bool dict;
                size_t idx;
            };
            std::vector<Job> jobs;
            jobs.push_back({false, true, 0});
            for (size_t i = 0; i < ec.size(); ++i) jobs.push_back({false, false, i});
            for (size_t i = 0; i < cc.size(); ++i) jobs.push_back({true, false, i});
            std::vector<MappedFile> emaps, cmaps;
            for (auto& f : event_files) emaps.push_back(use_map ? map_file(f) : MappedFile{});
            for (auto& f : click_files) cmaps.push_back(use_map ? map_file(f) : MappedFile{});
            std::vector<ChunkReader> readers(T);
            std::vector<std::vector<ClickTmp>> tmp(T);
            // Optional: ask the kernel to start reading a chunk several jobs ahead (ADVISE=1).
            // Off by default: it showed no gain where it could be measured.
            const bool advise = std::getenv("ADVISE") && std::atoi(std::getenv("ADVISE")) != 0;
            const size_t look = (size_t)T * 2;
            auto hint = [&](size_t j) {
                if (!advise || use_map || j >= jobs.size() || jobs[j].dict) return;
                const Job& jb = jobs[j];
                const Chunk& ch = jb.click ? cc[jb.idx] : ec[jb.idx];
                const InputFile& f = jb.click ? click_files[ch.file] : event_files[ch.file];
                ::posix_fadvise(f.fd, (off_t)ch.begin, (off_t)(ch.end - ch.begin), POSIX_FADV_WILLNEED);
            };
            parallel_jobs(T, jobs.size(), [&](int t, size_t j) {
                hint(j + look);
                const Job& job = jobs[j];
                if (job.dict) {
                    try {
                        dict.build(links, 1);
                    } catch (...) {
                        dict_state.store(2, std::memory_order_release);
                        throw;
                    }
                    dict_state.store(1, std::memory_order_release);
                } else if (!job.click) {
                    readers[t].for_each_record<7>(event_files[ec[job.idx].file], emaps[ec[job.idx].file], ec[job.idx],
                                                  [&](const char* p, size_t n, const Cols<7>& c) { parse_event_line(p, n, c, td[t], ic); });
                } else {
                    int st;
                    while ((st = dict_state.load(std::memory_order_acquire)) == 0) std::this_thread::yield();
                    if (st == 2) return;  // the build failed; its exception is rethrown by parallel_jobs
                    readers[t].for_each_record<4>(click_files[cc[job.idx].file], cmaps[cc[job.idx].file], cc[job.idx],
                                                  [&](const char* p, size_t n, const Cols<4>& c) { parse_click_line(p, n, c, td[t], ic, tmp[t]); });
                    emit_clicks(td[t], ic, tmp[t]);
                }
            });
            for (int t = 0; t < T; ++t) thread_stats().read_ns[t].fetch_add(readers[t].read_ns);
        }
        prof.count("link_ids", dict.ids());
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
        auto bits_for = [](u64 values) { return values > 1 ? 64u - (u32)__builtin_clzll(values - 1) : 0u; };
        ProcessContext pc{&products, &campaigns, &dict, base_day, ndays, p2_bits, win.start,
                          bits_for((u64)(win.end - win.start)), bits_for((u64)(win.end - win.start + WEEK_MS))};
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
#ifdef SS_PROFILE
        {
            Stopwatch w;
            u64 a = __rdtsc();
            while (std::chrono::duration<double>(std::chrono::steady_clock::now() - w.t0).count() < 0.2) {}
            double hz = (double)(__rdtsc() - a) / 0.2;
            const char* nm[] = {"dedup.table", "dedup.winners", "dedup.forward", "persons.number", "persons.sort",
                                "persons.loop", "  person.sessions", "  person.clicks+attr", "  person.emit", "(unused)",
                                "ev.split", "ev.ts+key", "ev.kind/price/prod", "ev.shuffle write", "click.parse(all)", "click.emit"};
            for (int i = 0; i < 16; ++i) std::fprintf(stderr, "CYC %-22s %.3fs\n", nm[i], g_cycles[i] / hz);
        }
#endif
        if (prof.verbose) std::fprintf(stderr, "TOTAL %.3fs\n", prof.total.lap());
        // Outputs are closed; skip tearing down gigabytes of working memory.
        std::fflush(stderr);
        ::_exit(0);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}
