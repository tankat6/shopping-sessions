// Strict CSV preprocessing.
#include <algorithm>
#include <array>
#include <atomic>
#include <thread>
#include <system_error>
#include <numeric>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using i64 = std::int64_t;
constexpr i64 WEEK_MS = 604800000LL;
constexpr const char* CR_MARKER = "\xEE\x80\x80";

bool utf8_ok(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { ++i; continue; }
        int n = (c >= 0xC2 && c <= 0xDF) ? 2 :
                (c >= 0xE0 && c <= 0xEF) ? 3 :
                (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
        if (!n || i + n > s.size()) return false;
        for (int j = 1; j < n; ++j)
            if (((unsigned char)s[i + j] & 0xC0) != 0x80) return false;
        unsigned char c1 = (unsigned char)s[i+1];
        if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 > 0x9F) ||
            (c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 > 0x8F)) return false;
        i += (size_t)n;
    }
    return true;
}

template<size_t N>
bool parse_csv(std::string_view line, std::array<std::string_view, N>& cells,
               bool validate_utf8) {
    if (line.empty()) return false;
    size_t start = 0;
    for (size_t i = 0; i < N; ++i) {
        size_t end = line.find(',', start);
        if (i + 1 < N) {
            if (end == std::string_view::npos) return false;
        } else {
            if (end != std::string_view::npos) return false;
            end = line.size();
        }
        auto cell = line.substr(start, end - start);
        if (cell.size() >= 2 && cell.front() == '"' && cell.back() == '"') {
            cell.remove_prefix(1);
            cell.remove_suffix(1);
        }
        if (cell.find('"') != std::string_view::npos) return false;
        if (validate_utf8 && !utf8_ok(cell)) return false;
        cells[i] = cell;
        start = end + 1;
    }
    return true;
}

bool digit(char x) { return x >= '0' && x <= '9'; }
int digit2(std::string_view s, size_t p) {
    if (p+1 >= s.size() || !digit(s[p]) || !digit(s[p+1])) return -1;
    return (s[p]-'0')*10+s[p+1]-'0';
}
bool leap(int y) { return y%4==0 && (y%100!=0 || y%400==0); }
int month_days(int y, int m) {
    static constexpr int d[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    if (m<1 || m>12) return 0;
    return d[m] + ((m==2 && leap(y)) ? 1 : 0);
}
// Convert calendar dates to epoch days.
i64 days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (i64)era * 146097 + (i64)doe - 719468;
}

bool parse_timestamp(std::string_view s, i64& millis) {
    if (s.empty()) return false;
    bool all_digits = true;
    for (char x : s) if (!digit(x)) { all_digits = false; break; }
    if (all_digits) {
        i64 value = 0;
        for (char x : s) {
            if (value > (INT64_MAX - (x-'0')) / 10) return false;
            value = value * 10 + (x-'0');
        }
        millis = value;
        return true;
    }
    if ((s.size() != 20 && s.size() != 25) ||
        s[4]!='-' || s[7]!='-' || s[10]!='T' ||
        s[13]!=':' || s[16]!=':') return false;
    for (int p : {0,1,2,3,5,6,8,9,11,12,14,15,17,18})
        if (!digit(s[p])) return false;
    int y = (s[0]-'0')*1000+(s[1]-'0')*100+(s[2]-'0')*10+(s[3]-'0');
    int m = digit2(s,5), d = digit2(s,8);
    int hh = digit2(s,11), mm = digit2(s,14), ss = digit2(s,17);
    if (y<1 || m<1 || m>12 || d<1 || d>month_days(y,m) ||
        hh>23 || mm>59 || ss>59) return false;
    int offset = 0;
    if (s.size() == 20) {
        if (s[19] != 'Z') return false;
    } else {
        if ((s[19]!='+' && s[19]!='-') || s[22]!=':') return false;
        int oh = digit2(s,20), om = digit2(s,23);
        if (oh < 0 || oh > 23 || om < 0 || om > 59) return false;
        offset = (oh*60+om) * (s[19]=='+' ? 1 : -1);
    }
    millis = days_from_civil(y,(unsigned)m,(unsigned)d)*86400000LL +
             (i64)hh*3600000 + (i64)mm*60000 + (i64)ss*1000 -
             (i64)offset*60000;
    return true;
}

std::vector<fs::path> csv_sources(const fs::path& dir) {
    std::vector<fs::path> ret;
    for (const auto& entry : fs::directory_iterator(dir))
        if (entry.is_regular_file() && entry.path().extension()==".csv") ret.push_back(entry.path());
    std::sort(ret.begin(), ret.end());
    return ret;
}

void write_protected(std::ostream& o, std::string_view s) {
    size_t p = 0;
    while (p < s.size()) {
        auto q = s.find('\r',p);
        if (q == std::string_view::npos) {o.write(s.data()+p, (std::streamsize)(s.size()-p)); return;}
        o.write(s.data()+p, (std::streamsize)(q-p));
        o.write(CR_MARKER,3);
        p = q+1;
    }
}

struct Job {
    fs::path source, destination;
    size_t index;
    uintmax_t bytes;
    bool event;
};
struct Counts { size_t kept = 0, dropped = 0; };

Counts process_job(const Job& job, i64 ws, i64 we) {
    std::vector<char> ibuf(1<<20), obuf(1<<20);
    std::ifstream in;
    in.rdbuf()->pubsetbuf(ibuf.data(), (std::streamsize)ibuf.size());
    in.open(job.source, std::ios::binary);
    std::ofstream out;
    out.rdbuf()->pubsetbuf(obuf.data(), (std::streamsize)obuf.size());
    out.open(job.destination, std::ios::binary);
    if (!in || !out) throw std::runtime_error("Cannot open job input or output: " + job.source.string());
    std::string line;
    std::getline(in,line);
    Counts stats;
    if (job.event) {
        out << "event_id,user_id,ts,ingest_ts,event_type,product_id,price_cents\n";
        std::array<std::string_view,7> fields;
        while (std::getline(in,line)) {
            if (!parse_csv<7>(line,fields,false)) { ++stats.dropped; continue; }
            write_protected(out,line);
            out.put('\n');
            ++stats.kept;
        }
    } else {
        out << "click_id,user_id,campaign_id,tms\n";
        std::array<std::string_view,4> click;
        while (std::getline(in,line)) {
            if (!parse_csv<4>(line,click,true) || click[0].empty() || click[1].empty()) {
                ++stats.dropped; continue;
            }
            i64 tms=0;
            if (!parse_timestamp(click[3],tms) || tms < ws-WEEK_MS || tms >= we) {
                ++stats.dropped; continue;
            }
            write_protected(out,click[0]); out.put(',');
            write_protected(out,click[1]); out.put(',');
            write_protected(out,click[2]); out.put(',');
            out << tms << '\n';
            ++stats.kept;
        }
    }
    out.flush();
    if (!out || in.bad()) throw std::runtime_error("Reading or writing CSV failed: " + job.source.string());
    return stats;
}

int main(int argc, char* argv[]) {
    if (argc != 5) {
        std::cerr << "usage: fast_csv DATA_DIR SCRATCH_DIR WINDOW_START_MS WINDOW_END_MS\n";
        return 2;
    }
    const fs::path data(argv[1]), scratch(argv[2]);
    i64 ws=0, we=0;
    try { ws=std::stoll(argv[3]); we=std::stoll(argv[4]); }
    catch (...) { std::cerr << "Bad window arguments\n"; return 2; }
    std::error_code ec;
    fs::create_directories(scratch / "verified_events",ec);
    if (ec) { std::cerr << ec.message() << '\n'; return 1; }
    fs::create_directories(scratch / "verified_clicks",ec);
    if (ec) { std::cerr << ec.message() << '\n'; return 1; }
    const auto events = csv_sources(data / "events");
    const auto clicks = csv_sources(data / "clicks");
    if (events.empty()) { std::cerr << "No event files found\n"; return 1; }
    std::vector<Job> jobs;
    jobs.reserve(events.size()+clicks.size());
    auto append = [&](const std::vector<fs::path>& files, bool event) {
        for (size_t i=0; i<files.size(); ++i) {
            char filename[64];
            std::snprintf(filename,sizeof(filename),"part-%06zu.csv",i);
            auto dest = scratch / (event ? "verified_events" : "verified_clicks") / filename;
            jobs.push_back({files[i],dest,jobs.size(),fs::file_size(files[i]),event});
        }
    };
    append(events,true);
    append(clicks,false);
    // Process large files first.
    std::sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.bytes > b.bytes; });
    std::vector<Counts> stats(jobs.size());
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    std::string failure_message;
    std::mutex failure_lock;
    int nthreads = 8;
    if (const char* env = std::getenv("NCPU")) {
        try { nthreads=std::max(1,std::min(8,std::stoi(env))); } catch (...) {}
    }
    nthreads=std::min(nthreads,(int)jobs.size());
    auto worker = [&]() {
        while (!failed.load(std::memory_order_relaxed)) {
            size_t j=next.fetch_add(1);
            if (j >= jobs.size()) return;
            try { stats[j]=process_job(jobs[j],ws,we); }
            catch (const std::exception& ex) {
                {
                    std::lock_guard<std::mutex> guard(failure_lock);
                    failure_message=ex.what();
                }
                failed.store(true);
                return;
            }
        }
    };
    std::vector<std::thread> threads;
    threads.reserve(nthreads);
    for(int t=0;t<nthreads;t++) threads.emplace_back(worker);
    for(auto& thread:threads) thread.join();
    if (failed) { std::cerr << failure_message << '\n'; return 1; }
    size_t event_kept=0,event_dropped=0, click_kept=0;
    for(size_t i=0;i<jobs.size();i++) {
        if (jobs[i].event) { event_kept+=stats[i].kept; event_dropped+=stats[i].dropped; }
        else click_kept+=stats[i].kept;
    }
    std::cerr << "Strict event CSV records: " << event_kept << " retained; "
              << event_dropped << " malformed discarded\n";
    std::cerr << "Exact-valid click rows: " << click_kept << '\n';
    return 0;
}
