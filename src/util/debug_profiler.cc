#include "util/debug_profiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "util/logger.h"

namespace {

using SteadyClock = std::chrono::steady_clock;

uint64_t NowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        SteadyClock::now().time_since_epoch()).count());
}

bool ParseEnvFlag(const char* value) {
    if (!value) return false;
    if (std::strcmp(value, "1") == 0) return true;
    if (std::strcmp(value, "true") == 0) return true;
    if (std::strcmp(value, "TRUE") == 0) return true;
    if (std::strcmp(value, "on") == 0) return true;
    if (std::strcmp(value, "ON") == 0) return true;
    if (std::strcmp(value, "yes") == 0) return true;
    if (std::strcmp(value, "YES") == 0) return true;
    return false;
}

uint64_t ParseEnvU64(const char* name, uint64_t fallback) {
    const char* v = std::getenv(name);
    if (!v || *v == '\0') return fallback;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(v, &end, 10);
    if (end == v) return fallback;
    return static_cast<uint64_t>(parsed);
}

std::string DetectDefaultProfilerDir() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    if (ec) return "analysis/profiler";

    fs::path root = cwd;
    if (!fs::exists(root / "CMakeLists.txt")) {
        if (fs::exists(root / ".." / "CMakeLists.txt")) {
            root = root / "..";
        } else if (fs::exists(root / ".." / ".." / "CMakeLists.txt")) {
            root = root / ".." / "..";
        }
    }
    return (root / "analysis" / "profiler").lexically_normal().string();
}

struct ScopeStats {
    std::string name;
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> total_ns{0};
    std::atomic<uint64_t> max_ns{0};
};

struct SnapshotRow {
    std::string name;
    uint64_t calls = 0;
    uint64_t total_ns = 0;
    uint64_t max_ns = 0;
};

class DebugProfilerState {
 public:
    bool enabled = false;
    uint64_t min_log_interval_ns = 0;
    uint64_t next_log_deadline_ns = 0;
    uint64_t top_n = 16;
    uint64_t min_row_ns = 0;
    std::string output_dir;
    std::string history_path;
    std::string latest_path;
    bool io_warning_emitted = false;

    std::mutex map_mu;
    std::unordered_map<std::string, uint64_t> name_to_id;
    std::deque<ScopeStats> stats;

    std::mutex log_mu;
};

DebugProfilerState& GetState() {
    static DebugProfilerState state;
    return state;
}

}  // namespace

DebugProfiler::DebugProfiler() {
    DebugProfilerState& s = GetState();
    s.enabled = ParseEnvFlag(std::getenv("AXOLOTL_CPU_PROFILE"));
    if (!s.enabled) return;

    const uint64_t interval_ms = ParseEnvU64("AXOLOTL_CPU_PROFILE_INTERVAL_MS", 2000);
    s.min_log_interval_ns = interval_ms * 1000000ull;
    s.next_log_deadline_ns = NowNs() + s.min_log_interval_ns;
    s.top_n = ParseEnvU64("AXOLOTL_CPU_PROFILE_TOP", 16);
    if (s.top_n == 0) s.top_n = 16;
    const uint64_t min_row_us = ParseEnvU64("AXOLOTL_CPU_PROFILE_MIN_US", 100);
    s.min_row_ns = min_row_us * 1000ull;
    const char* out_dir = std::getenv("AXOLOTL_CPU_PROFILE_DIR");
    if (out_dir && *out_dir) {
        s.output_dir = out_dir;
    } else if (const char* root = std::getenv("AXOLOTL_ROOT"); root && *root) {
        s.output_dir = (std::filesystem::path(root) / "analysis" / "profiler").string();
    } else {
        s.output_dir = DetectDefaultProfilerDir();
    }
    s.history_path = s.output_dir + "/cpu_profile.log";
    s.latest_path = s.output_dir + "/cpu_profile_latest.log";
    std::error_code ec;
    std::filesystem::create_directories(s.output_dir, ec);
    if (ec) {
        Logger::log("[Profiler] failed to create output dir: " + s.output_dir +
                    " err=" + ec.message(), LogLevel::WARNING);
    }

    std::ostringstream msg;
    msg << "[Profiler] enabled interval_ms=" << interval_ms
        << " top=" << s.top_n
        << " min_row_us=" << min_row_us
        << " output_dir=" << s.output_dir;
    Logger::log(msg.str(), LogLevel::INFO);
}

DebugProfiler& DebugProfiler::Instance() {
    static DebugProfiler instance;
    return instance;
}

bool DebugProfiler::Enabled() const {
    return GetState().enabled;
}

uint64_t DebugProfiler::RegisterScope(const char* scope_name) {
    DebugProfilerState& s = GetState();
    if (!s.enabled || !scope_name) return 0;

    std::lock_guard<std::mutex> lock(s.map_mu);
    const auto it = s.name_to_id.find(scope_name);
    if (it != s.name_to_id.end()) return it->second;

    const uint64_t id = static_cast<uint64_t>(s.stats.size());
    s.name_to_id.emplace(scope_name, id);
    s.stats.emplace_back();
    s.stats.back().name = scope_name;
    return id;
}

void DebugProfiler::AddSample(uint64_t scope_id, uint64_t elapsed_ns) {
    DebugProfilerState& s = GetState();
    if (!s.enabled) return;

    ScopeStats* stat = nullptr;
    {
        std::lock_guard<std::mutex> lock(s.map_mu);
        if (scope_id < s.stats.size()) stat = &s.stats[static_cast<size_t>(scope_id)];
    }
    if (!stat) return;

    stat->calls.fetch_add(1, std::memory_order_relaxed);
    stat->total_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);

    uint64_t old_max = stat->max_ns.load(std::memory_order_relaxed);
    while (elapsed_ns > old_max &&
           !stat->max_ns.compare_exchange_weak(old_max, elapsed_ns,
                                               std::memory_order_relaxed,
                                               std::memory_order_relaxed)) {
    }
}

void DebugProfiler::MaybeLogPeriodic() {
    DebugProfilerState& s = GetState();
    if (!s.enabled || s.min_log_interval_ns == 0) return;

    const uint64_t now_ns = NowNs();
    if (now_ns < s.next_log_deadline_ns) return;

    std::lock_guard<std::mutex> lock(s.log_mu);
    if (now_ns < s.next_log_deadline_ns) return;
    s.next_log_deadline_ns = now_ns + s.min_log_interval_ns;
    LogSummary("periodic");
}

void DebugProfiler::LogSummary(const char* reason) {
    DebugProfilerState& s = GetState();
    if (!s.enabled) return;

    std::vector<SnapshotRow> rows;
    rows.reserve(s.stats.size());
    uint64_t grand_total_ns = 0;

    {
        std::lock_guard<std::mutex> lock(s.map_mu);
        for (const ScopeStats& st : s.stats) {
            SnapshotRow row{};
            row.name = st.name;
            row.calls = st.calls.load(std::memory_order_relaxed);
            row.total_ns = st.total_ns.load(std::memory_order_relaxed);
            row.max_ns = st.max_ns.load(std::memory_order_relaxed);
            grand_total_ns += row.total_ns;
            if (row.total_ns >= s.min_row_ns || row.calls != 0) {
                rows.push_back(std::move(row));
            }
        }
    }

    std::sort(rows.begin(), rows.end(), [](const SnapshotRow& a, const SnapshotRow& b) {
        if (a.total_ns != b.total_ns) return a.total_ns > b.total_ns;
        return a.calls > b.calls;
    });

    if (rows.empty() || grand_total_ns == 0) return;

    if (rows.size() > s.top_n) rows.resize(static_cast<size_t>(s.top_n));

    std::ostringstream msg;
    msg.setf(std::ios::fixed);
    msg.precision(2);
    msg << "[Profiler] summary";
    if (reason && *reason != '\0') msg << " (" << reason << ")";
    msg << "\n";

    for (const SnapshotRow& row : rows) {
        const double total_ms = static_cast<double>(row.total_ns) / 1000000.0;
        const double avg_us = row.calls == 0
            ? 0.0
            : static_cast<double>(row.total_ns) / (1000.0 * static_cast<double>(row.calls));
        const double max_us = static_cast<double>(row.max_ns) / 1000.0;
        const double pct = grand_total_ns == 0
            ? 0.0
            : (100.0 * static_cast<double>(row.total_ns) / static_cast<double>(grand_total_ns));

        msg << "  " << row.name
            << " | total_ms=" << total_ms
            << " | pct=" << pct
            << " | calls=" << row.calls
            << " | avg_us=" << avg_us
            << " | max_us=" << max_us
            << "\n";
    }

    const std::string text = msg.str();
    Logger::log(text, LogLevel::INFO);

    if (!s.output_dir.empty()) {
        bool write_ok = true;
        {
            std::ofstream latest(s.latest_path, std::ios::trunc);
            if (latest.is_open()) {
                latest << text;
            } else {
                write_ok = false;
            }
        }
        {
            std::ofstream history(s.history_path, std::ios::app);
            if (history.is_open()) {
                history << text << '\n';
            } else {
                write_ok = false;
            }
        }
        if (!write_ok && !s.io_warning_emitted) {
            s.io_warning_emitted = true;
            Logger::log("[Profiler] failed to write profile logs to: " + s.output_dir,
                        LogLevel::WARNING);
        }
    }
}

DebugProfiler::Scope::Scope(uint64_t scope_id) {
    DebugProfiler& profiler = DebugProfiler::Instance();
    enabled_ = profiler.Enabled();
    if (!enabled_) return;
    scope_id_ = scope_id;
    start_ns_ = NowNs();
}

DebugProfiler::Scope::~Scope() {
    if (!enabled_) return;
    const uint64_t end_ns = NowNs();
    const uint64_t elapsed_ns = end_ns >= start_ns_ ? (end_ns - start_ns_) : 0;
    DebugProfiler& profiler = DebugProfiler::Instance();
    profiler.AddSample(scope_id_, elapsed_ns);
    profiler.MaybeLogPeriodic();
}
