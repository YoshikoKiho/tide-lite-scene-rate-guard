#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <map>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kMaxClusters = 4;
constexpr uint64_t kMaxRateUs = 1000000ULL;
constexpr size_t kMaxLogBytes = 65536;

struct Paths {
    std::string data_dir = "/data/adb/chaoxi";
    std::string cpu_root = "/sys/devices/system/cpu/cpufreq";
    std::string pwr_root = "/sys/class/power_supply";
    std::string scene_profile = "/data/user/0/com.omarea.vtools/files/profile.json";
    std::string scene_obj_dir = "/data/user/0/com.omarea.vtools/files/objects";
    std::string conf_file;
    std::string state_file;
    std::string log_file;
    std::string pid_file;
    std::string lock_file;
    std::string ui_file;

    void init() {
        if (const char* e = std::getenv("CHAOXI_DATA")) data_dir = e;
        if (const char* e = std::getenv("CHAOXI_CPU"))  cpu_root = e;
        if (const char* e = std::getenv("CHAOXI_PWR"))  pwr_root = e;
        if (const char* e = std::getenv("CHAOXI_SCENE_PROFILE")) scene_profile = e;
        if (const char* e = std::getenv("CHAOXI_SCENE_OBJ")) scene_obj_dir = e;
        conf_file  = data_dir + "/config.conf";
        state_file = data_dir + "/state.conf";
        log_file   = data_dir + "/tide.log";
        pid_file   = data_dir + "/daemon.pid";
        lock_file  = data_dir + "/daemon.lock";
        ui_file    = data_dir + "/ui.json";
    }
};

struct UniqueFd {
    int fd = -1;
    UniqueFd() = default;
    explicit UniqueFd(int f) : fd(f) {}
    ~UniqueFd() { reset(); }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& o) noexcept : fd(o.fd) { o.fd = -1; }
    UniqueFd& operator=(UniqueFd&& o) noexcept {
        if (this != &o) { reset(o.fd); o.fd = -1; }
        return *this;
    }
    void reset(int f = -1) {
        if (fd >= 0) ::close(fd);
        fd = f;
    }
    int get() const { return fd; }
    bool valid() const { return fd >= 0; }
};

int64_t mono_ms() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::string now_str() {
    time_t t = time(nullptr);
    struct tm tm_buf {};
    localtime_r(&t, &tm_buf);
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d-%02d %02d:%02d:%02d",
             tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return buf;
}

std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool read_file_str(const std::string& path, std::string& out, size_t max_bytes = 65536) {
    UniqueFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (!fd.valid()) return false;
    out.clear();
    char buf[4096];
    while (out.size() < max_bytes) {
        ssize_t n = ::read(fd.get(), buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return true;
}

bool read_uint(const std::string& path, uint64_t& val) {
    std::string s;
    if (!read_file_str(path, s, 64)) return false;
    s = trim(s);
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    unsigned long long v = strtoull(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0') return false;
    val = static_cast<uint64_t>(v);
    return true;
}

bool write_uint_sysfs(const std::string& path, uint64_t val) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%llu\n", static_cast<unsigned long long>(val));
    if (len <= 0) return false;
    UniqueFd fd(::open(path.c_str(), O_WRONLY | O_CLOEXEC));
    bool restored_mode = false;
    mode_t prev_mode = 0664;
    if (!fd.valid() && (errno == EACCES || errno == EPERM)) {
        struct stat st {};
        if (::stat(path.c_str(), &st) == 0) {
            prev_mode = st.st_mode & 0777;
            if (::chmod(path.c_str(), 0664) == 0) {
                restored_mode = true;
                fd.reset(::open(path.c_str(), O_WRONLY | O_CLOEXEC));
            }
        }
    }
    if (!fd.valid()) {
        if (restored_mode) ::chmod(path.c_str(), prev_mode);
        return false;
    }
    ssize_t n = ::write(fd.get(), buf, static_cast<size_t>(len));
    fd.reset();
    if (restored_mode) ::chmod(path.c_str(), prev_mode);
    if (n <= 0) return false;
    uint64_t verify = 0;
    return read_uint(path, verify) && verify == val;
}

bool is_path_shielded(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && (st.st_mode & 0222) == 0;
}

std::string format_path_mode(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return "";
    char buf[8];
    snprintf(buf, sizeof(buf), "%04o", static_cast<unsigned>(st.st_mode & 0777));
    return buf;
}

bool mode_uses_shield(const std::string& mode) {
    return mode == "override" || mode == "shield";
}

bool write_atomic_file(const std::string& path, const std::string& content, mode_t mode = 0600) {
    std::string tmp = path + ".tmp." + std::to_string(::getpid());
    UniqueFd fd(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode));
    if (!fd.valid()) return false;
    size_t off = 0;
    while (off < content.size()) {
        ssize_t n = ::write(fd.get(), content.data() + off, content.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::unlink(tmp.c_str());
            return false;
        }
        off += static_cast<size_t>(n);
    }
    ::fchmod(fd.get(), mode);
    fd.reset();
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

bool write_atomic_preserve(const std::string& path, const std::string& content, mode_t fallback_mode = 0600) {
    struct stat st {};
    bool have_stat = (::stat(path.c_str(), &st) == 0);
    mode_t mode = have_stat ? (st.st_mode & 0777) : fallback_mode;
    std::string tmp = path + ".tmp." + std::to_string(::getpid());
    UniqueFd fd(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode));
    if (!fd.valid()) return false;
    size_t off = 0;
    while (off < content.size()) {
        ssize_t n = ::write(fd.get(), content.data() + off, content.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::unlink(tmp.c_str());
            return false;
        }
        off += static_cast<size_t>(n);
    }
    ::fchmod(fd.get(), mode);
    if (have_stat) {
        if (::fchown(fd.get(), st.st_uid, st.st_gid) != 0) {
            // ignore on non-root mock environments
        }
    }
    fd.reset();
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

void init_local_tz_once() {
    static bool inited = false;
    if (inited) return;
    inited = true;
    const char* cur_tz = ::getenv("TZ");
    if (cur_tz && *cur_tz) {
        ::tzset();
        return;
    }
    FILE* fp = ::popen("date +%z 2>/dev/null", "r");
    if (!fp) return;
    char buf[32] = {};
    std::string out;
    if (std::fgets(buf, sizeof(buf), fp)) out = buf;
    ::pclose(fp);
    // Trim whitespace
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    // Parse [+-]HHMM -> POSIX TZ (sign flipped: +0800 => UTC-08:00)
    if (out.size() == 5 && (out[0] == '+' || out[0] == '-') &&
        std::isdigit(static_cast<unsigned char>(out[1])) &&
        std::isdigit(static_cast<unsigned char>(out[2])) &&
        std::isdigit(static_cast<unsigned char>(out[3])) &&
        std::isdigit(static_cast<unsigned char>(out[4]))) {
        char inv = (out[0] == '+') ? '-' : '+';
        std::string posix_tz = std::string("UTC") + inv + out.substr(1, 2) + ":" + out.substr(3, 2);
        ::setenv("TZ", posix_tz.c_str(), 1);
        ::tzset();
    }
}

bool file_exists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

bool dir_exists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void ensure_dir(const std::string& path, mode_t mode = 0700) {
    ::mkdir(path.c_str(), mode);
    ::chmod(path.c_str(), mode);
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(static_cast<char>(c)); }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out.push_back(static_cast<char>(c));
    }
    return out;
}

std::string get_prop(const char* name) {
    // Minimal property read via /dev/__properties__ isn't portable; use getprop fallback only in boot wait / status.
    std::string cmd = std::string("getprop ") + name + " 2>/dev/null";
    FILE* fp = ::popen(cmd.c_str(), "r");
    if (!fp) return "";
    char buf[128] = {};
    std::string res;
    if (fgets(buf, sizeof(buf), fp)) res = trim(buf);
    ::pclose(fp);
    return res;
}

struct Config {
    bool enabled = true;
    std::string mode = "override"; // "override" (default 0444 lock), "shield" (0444 Scene passthrough), "compat" (0664 boost-exempt)
    int low_bat_threshold = 20;
    bool watch_charger = true;
    bool watch_screen = true;
    uint64_t up[kMaxClusters] = {8000, 4000, 1000, 1000};
    uint64_t dn[kMaxClusters] = {8000, 4000, 1000, 1000};
};

struct State {
    uint64_t fixes = 0;
    int64_t last_fix_ts = 0;
    std::string last_reason;
    bool dirty = false;
    int64_t last_flush_ms = 0;
};

struct Cluster {
    int index = 0;
    int policy_num = 0;
    std::string policy_name;
    std::string policy_dir;
    std::string cpus;
    std::vector<int> cpu_ids;
    uint64_t max_khz = 0;
    std::string governor;
    std::string rate_dir;
    std::string up_path;
    std::string dn_path;

    int wd_dir = -1;
    int wd_gov = -1;
    int wd_up = -1;
    int wd_dn = -1;
    ino_t wd_up_ino = 0;
    ino_t wd_dn_ino = 0;
    uint32_t wd_up_mask = 0;
    uint32_t wd_dn_mask = 0;

    int64_t burst_start_ms = 0;
    int burst_count = 0;
    int64_t suppress_until_ms = 0;
};

struct Logger {
    std::string path;
    int64_t fix_window_ms = 0;
    int fix_window_lines = 0;
    int suppressed_lines = 0;

    void write(const std::string& msg) {
        if (path.empty()) return;
        struct stat st {};
        if (::stat(path.c_str(), &st) == 0 && static_cast<size_t>(st.st_size) > kMaxLogBytes) {
            std::string all;
            if (read_file_str(path, all, kMaxLogBytes * 2)) {
                size_t cut = all.size() / 2;
                size_t nl = all.find('\n', cut);
                std::string tail = (nl != std::string::npos) ? all.substr(nl + 1) : all.substr(cut);
                write_atomic_file(path, tail, 0600);
            }
        }
        UniqueFd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600));
        if (!fd.valid()) return;
        std::string line = now_str() + " " + msg + "\n";
        (void)::write(fd.get(), line.data(), line.size());
    }

    void write_fix_throttled(const std::string& msg) {
        int64_t now_m = mono_ms();
        if (fix_window_ms == 0 || now_m - fix_window_ms >= 60000) {
            if (suppressed_lines > 0) {
                write("summary: +" + std::to_string(suppressed_lines) + " fix logs folded in last 60s");
                suppressed_lines = 0;
            }
            fix_window_ms = now_m;
            fix_window_lines = 0;
        }
        if (fix_window_lines < 12) {
            ++fix_window_lines;
            write(msg);
        } else {
            ++suppressed_lines;
        }
    }
};

std::vector<int> parse_cpu_list(const std::string& s) {
    std::vector<int> ids;
    std::istringstream iss(s);
    int v = 0;
    while (iss >> v) ids.push_back(v);
    return ids;
}

std::string format_cpus_compact(const std::vector<int>& ids) {
    if (ids.empty()) return "";
    std::string out;
    size_t i = 0;
    while (i < ids.size()) {
        size_t j = i;
        while (j + 1 < ids.size() && ids[j + 1] == ids[j] + 1) ++j;
        if (!out.empty()) out += ",";
        if (i == j) out += std::to_string(ids[i]);
        else out += std::to_string(ids[i]) + "-" + std::to_string(ids[j]);
        i = j + 1;
    }
    return out;
}

void refresh_cluster_rate_paths(Cluster& c) {
    std::string gov;
    if (read_file_str(c.policy_dir + "/scaling_governor", gov, 128))
        c.governor = trim(gov);
    else
        c.governor.clear();

    c.rate_dir.clear();
    c.up_path.clear();
    c.dn_path.clear();

    std::vector<std::string> candidates;
    if (!c.governor.empty()) candidates.push_back(c.governor);
    candidates.push_back("sugov_ext");
    candidates.push_back("schedutil");
    candidates.push_back("");

    for (const auto& sub : candidates) {
        std::string dir = sub.empty() ? c.policy_dir : (c.policy_dir + "/" + sub);
        std::string up = dir + "/up_rate_limit_us";
        std::string dn = dir + "/down_rate_limit_us";
        if (file_exists(up) && file_exists(dn)) {
            c.rate_dir = sub.empty() ? "root" : sub;
            c.up_path = up;
            c.dn_path = dn;
            break;
        }
    }
}

std::vector<Cluster> discover_clusters(const Paths& paths) {
    std::vector<Cluster> out;
    DIR* d = ::opendir(paths.cpu_root.c_str());
    if (!d) return out;
    struct dirent* ent = nullptr;
    while ((ent = ::readdir(d)) != nullptr) {
        if (std::strncmp(ent->d_name, "policy", 6) != 0) continue;
        const char* num_p = ent->d_name + 6;
        if (*num_p == '\0') continue;
        bool all_digit = true;
        for (const char* p = num_p; *p; ++p) {
            if (*p < '0' || *p > '9') { all_digit = false; break; }
        }
        if (!all_digit) continue;
        std::string pdir = paths.cpu_root + "/" + ent->d_name;
        if (!dir_exists(pdir)) continue;

        Cluster c;
        c.policy_name = ent->d_name;
        c.policy_num = std::atoi(num_p);
        c.policy_dir = pdir;
        uint64_t mf = 0;
        if (read_uint(pdir + "/cpuinfo_max_freq", mf)) c.max_khz = mf;
        std::string rel;
        if (read_file_str(pdir + "/related_cpus", rel, 128) ||
            read_file_str(pdir + "/affected_cpus", rel, 128)) {
            c.cpu_ids = parse_cpu_list(rel);
            c.cpus = format_cpus_compact(c.cpu_ids);
        }
        refresh_cluster_rate_paths(c);
        out.push_back(std::move(c));
    }
    ::closedir(d);

    std::sort(out.begin(), out.end(), [](const Cluster& a, const Cluster& b) {
        if (a.max_khz != b.max_khz) return a.max_khz < b.max_khz;
        return a.policy_num < b.policy_num;
    });
    if (out.size() > kMaxClusters) out.resize(kMaxClusters);
    for (size_t i = 0; i < out.size(); ++i) out[i].index = static_cast<int>(i);
    return out;
}

bool cluster_owns_cpu(const Cluster& c, int id) {
    if (c.policy_num == id) return true;
    return std::find(c.cpu_ids.begin(), c.cpu_ids.end(), id) != c.cpu_ids.end();
}

struct SceneRates {
    bool found = false;
    bool has_up[kMaxClusters] = {};
    bool has_dn[kMaxClusters] = {};
    uint64_t up[kMaxClusters] = {};
    uint64_t dn[kMaxClusters] = {};
};

struct SceneAllSchemes {
    SceneRates powersave;
    SceneRates balance;
    SceneRates performance;
};

int extract_cpu_num_from_path(const std::string& prefix) {
    auto find_last_num = [&](const char* pat, size_t plen) -> std::pair<size_t, int> {
        size_t end_lim = prefix.size();
        while (end_lim > 0) {
            size_t p = prefix.rfind(pat, end_lim - 1);
            if (p == std::string::npos) break;
            if (p + plen < prefix.size()) {
                char ch = prefix[p + plen];
                if (ch >= '0' && ch <= '9') {
                    return {p, std::atoi(prefix.c_str() + p + plen)};
                }
            }
            end_lim = p;
        }
        return {std::string::npos, -1};
    };
    auto pr = find_last_num("/policy", 7);
    auto cr = find_last_num("/cpu", 4);
    if (pr.first != std::string::npos && (cr.first == std::string::npos || pr.first > cr.first)) {
        return pr.second;
    }
    if (cr.first != std::string::npos) return cr.second;
    return -1;
}

bool find_json_container_range(const std::string& raw, size_t range_start, size_t range_end,
                               const char* key_name, size_t& out_start, size_t& out_end,
                               bool require_rates = false) {
    std::string key = std::string("\"") + key_name + "\"";
    size_t search = range_start;
    while (search < range_end) {
        size_t pos = raw.find(key, search);
        if (pos == std::string::npos || pos >= range_end) break;
        search = pos + key.size();
        size_t p = search;
        while (p < range_end && (raw[p] == ' ' || raw[p] == '\t' || raw[p] == '\r' || raw[p] == '\n')) ++p;
        if (p >= range_end || raw[p] != ':') continue;
        ++p;
        while (p < range_end && (raw[p] == ' ' || raw[p] == '\t' || raw[p] == '\r' || raw[p] == '\n')) ++p;
        if (p >= range_end || (raw[p] != '{' && raw[p] != '[')) continue;
        char open_c = raw[p];
        char close_c = (open_c == '{') ? '}' : ']';
        int depth = 0;
        bool in_str = false;
        size_t end_pos = range_end;
        for (size_t k = p; k < range_end; ++k) {
            char ch = raw[k];
            if (in_str) {
                if (ch == '\\' && k + 1 < range_end) { ++k; continue; }
                if (ch == '"') in_str = false;
            } else {
                if (ch == '"') in_str = true;
                else if (ch == open_c) ++depth;
                else if (ch == close_c && --depth == 0) { end_pos = k + 1; break; }
            }
        }
        if (end_pos <= range_end) {
            if (!require_rates || raw.find("rate_limit_us", p) < end_pos) {
                out_start = p;
                out_end = end_pos;
                return true;
            }
        }
    }
    return false;
}

SceneRates parse_scene_slice(const std::string& raw, size_t slice_start, size_t slice_end,
                             const std::vector<Cluster>& clusters) {
    SceneRates sr;
    size_t pos = slice_start;
    while (pos < slice_end) {
        size_t hit = raw.find("rate_limit_us", pos);
        if (hit == std::string::npos || hit >= slice_end) break;
        pos = hit + 13;

        bool is_up = false, is_dn = false;
        if (hit >= slice_start + 3 && raw.compare(hit - 3, 3, "up_") == 0) is_up = true;
        else if (hit >= slice_start + 5 && raw.compare(hit - 5, 5, "down_") == 0) is_dn = true;
        if (!is_up && !is_dn) continue;

        size_t back_start = (hit > slice_start + 96) ? (hit - 96) : slice_start;
        int cpu_num = extract_cpu_num_from_path(raw.substr(back_start, hit - back_start));
        if (cpu_num < 0) continue;

        size_t q1 = raw.find('"', hit);
        if (q1 == std::string::npos || q1 >= slice_end) continue;
        size_t q2 = raw.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 >= slice_end) continue;
        size_t q3 = raw.find('"', q2 + 1);
        if (q3 == std::string::npos || q3 >= slice_end || q3 - q2 > 32) continue;
        std::string val_s = trim(raw.substr(q2 + 1, q3 - q2 - 1));
        if (val_s.empty()) continue;
        bool digits = std::all_of(val_s.begin(), val_s.end(), [](unsigned char ch) { return ch >= '0' && ch <= '9'; });
        if (!digits) continue;
        uint64_t val = strtoull(val_s.c_str(), nullptr, 10);
        if (val > kMaxRateUs) continue;

        for (size_t i = 0; i < clusters.size(); ++i) {
            if (cluster_owns_cpu(clusters[i], cpu_num)) {
                if (is_up) { sr.up[i] = val; sr.has_up[i] = true; sr.found = true; }
                if (is_dn) { sr.dn[i] = val; sr.has_dn[i] = true; sr.found = true; }
                break;
            }
        }
    }
    return sr;
}

SceneRates parse_scene_profile(const Paths& paths, const std::vector<Cluster>& clusters,
                               const char* preferred_scheme = nullptr) {
    SceneRates sr;
    std::string raw;
    if (!read_file_str(paths.scene_profile, raw, 512 * 1024)) return sr;

    size_t root_s = 0, root_e = raw.size();
    size_t schemes_s = 0, schemes_e = raw.size();
    if (find_json_container_range(raw, 0, raw.size(), "schemes", schemes_s, schemes_e)) {
        root_s = schemes_s;
        root_e = schemes_e;
    }

    if (preferred_scheme && *preferred_scheme) {
        size_t cs = 0, ce = 0;
        if (find_json_container_range(raw, root_s, root_e, preferred_scheme, cs, ce, true)) {
            return parse_scene_slice(raw, cs, ce, clusters);
        }
        return sr;
    }

    for (const char* key : {"balance", "performance", "powersave"}) {
        size_t cs = 0, ce = 0;
        if (find_json_container_range(raw, root_s, root_e, key, cs, ce, true)) {
            return parse_scene_slice(raw, cs, ce, clusters);
        }
    }
    return parse_scene_slice(raw, 0, raw.size(), clusters);
}

SceneAllSchemes parse_all_scene_schemes(const Paths& paths, const std::vector<Cluster>& clusters) {
    SceneAllSchemes all;
    std::string raw;
    if (!read_file_str(paths.scene_profile, raw, 512 * 1024)) return all;
    size_t root_s = 0, root_e = raw.size();
    size_t schemes_s = 0, schemes_e = raw.size();
    if (find_json_container_range(raw, 0, raw.size(), "schemes", schemes_s, schemes_e)) {
        root_s = schemes_s;
        root_e = schemes_e;
    }
    size_t cs = 0, ce = 0;
    if (find_json_container_range(raw, root_s, root_e, "powersave", cs, ce, true))
        all.powersave = parse_scene_slice(raw, cs, ce, clusters);
    if (find_json_container_range(raw, root_s, root_e, "balance", cs, ce, true))
        all.balance = parse_scene_slice(raw, cs, ce, clusters);
    if (find_json_container_range(raw, root_s, root_e, "performance", cs, ce, true))
        all.performance = parse_scene_slice(raw, cs, ce, clusters);
    return all;
}

bool patch_scene_object_bin(const std::string& obj_path, const std::vector<Cluster>& clusters, const Config& cfg) {
    std::string bin;
    if (!read_file_str(obj_path, bin, 256 * 1024)) return false;
    if (bin.size() < 6 || static_cast<unsigned char>(bin[0]) != 0xAC ||
        static_cast<unsigned char>(bin[1]) != 0xED) {
        return false;
    }

    struct BinPatch { size_t pos; size_t len; std::string repl; };
    std::vector<BinPatch> patches;
    const std::string needle = "rate_limit_us";
    size_t pos = 0;
    while (pos < bin.size()) {
        size_t hit = bin.find(needle, pos);
        if (hit == std::string::npos) break;
        pos = hit + needle.size();

        bool is_up = (hit >= 3 && bin.compare(hit - 3, 3, "up_") == 0);
        bool is_dn = (hit >= 5 && bin.compare(hit - 5, 5, "down_") == 0);
        if (!is_up && !is_dn) continue;

        size_t key_end = hit + needle.size();
        // Locate enclosing Java TC_STRING (0x74 + uint16_be len + path)
        size_t tc_pos = std::string::npos;
        size_t scan_min = (hit > 96) ? (hit - 96) : 0;
        for (size_t k = hit; k > scan_min; --k) {
            size_t idx = k - 1;
            if (static_cast<unsigned char>(bin[idx]) == 0x74 && idx + 3 <= hit) {
                uint16_t plen = (static_cast<uint16_t>(static_cast<unsigned char>(bin[idx + 1])) << 8) |
                                 static_cast<uint16_t>(static_cast<unsigned char>(bin[idx + 2]));
                if (plen > 10 && plen < 160 && idx + 3 + plen == key_end) {
                    tc_pos = idx;
                    break;
                }
            }
        }
        if (tc_pos == std::string::npos) continue;

        std::string key_str = bin.substr(tc_pos + 3, key_end - (tc_pos + 3));
        if (key_str.find("/sys/") == std::string::npos) continue;
        int cpu_num = extract_cpu_num_from_path(key_str);
        if (cpu_num < 0) continue;

        int c_idx = -1;
        for (size_t i = 0; i < clusters.size(); ++i) {
            if (cluster_owns_cpu(clusters[i], cpu_num)) { c_idx = static_cast<int>(i); break; }
        }
        if (c_idx < 0) continue;

        size_t vpos = key_end;
        if (vpos + 3 > bin.size() || static_cast<unsigned char>(bin[vpos]) != 0x74) continue;
        uint16_t vlen = (static_cast<uint16_t>(static_cast<unsigned char>(bin[vpos + 1])) << 8) |
                         static_cast<uint16_t>(static_cast<unsigned char>(bin[vpos + 2]));
        if (vlen == 0 || vlen > 16 || vpos + 3 + vlen > bin.size()) continue;
        bool all_digits = true;
        for (size_t j = 0; j < vlen; ++j) {
            unsigned char ch = static_cast<unsigned char>(bin[vpos + 3 + j]);
            if (ch < '0' || ch > '9') { all_digits = false; break; }
        }
        if (!all_digits) continue;

        uint64_t val = is_up ? cfg.up[c_idx] : cfg.dn[c_idx];
        std::string vstr = std::to_string(val);
        std::string repl;
        repl.reserve(3 + vstr.size());
        repl.push_back(static_cast<char>(0x74));
        repl.push_back(static_cast<char>((vstr.size() >> 8) & 0xFF));
        repl.push_back(static_cast<char>(vstr.size() & 0xFF));
        repl += vstr;
        patches.push_back({vpos, static_cast<size_t>(3 + vlen), std::move(repl)});
    }
    if (patches.empty()) return false;
    std::sort(patches.begin(), patches.end(), [](const BinPatch& a, const BinPatch& b) { return a.pos > b.pos; });
    for (const auto& p : patches) {
        bin.replace(p.pos, p.len, p.repl);
    }
    return write_atomic_preserve(obj_path, bin, 0600);
}

bool write_scene_schemes(const Paths& paths, const std::vector<Cluster>& clusters, const Config& cfg,
                         const std::string& scheme_arg, std::string& synced_out) {
    synced_out.clear();
    if (scheme_arg.empty() || scheme_arg == "none") return true;

    std::vector<std::string> targets;
    if (scheme_arg == "all") {
        targets = {"powersave", "balance", "performance"};
    } else if (scheme_arg == "powersave" || scheme_arg == "balance" || scheme_arg == "performance") {
        targets.push_back(scheme_arg);
    } else {
        return false;
    }

    std::string raw;
    if (!read_file_str(paths.scene_profile, raw, 512 * 1024) || raw.empty()) return false;

    size_t root_s = 0, root_e = raw.size();
    size_t schemes_s = 0, schemes_e = raw.size();
    if (find_json_container_range(raw, 0, raw.size(), "schemes", schemes_s, schemes_e)) {
        root_s = schemes_s;
        root_e = schemes_e;
    }

    struct JsonPatch { size_t start; size_t end; std::string repl; };
    std::vector<JsonPatch> patches;
    std::vector<std::string> updated_schemes;

    for (const auto& name : targets) {
        size_t cs = 0, ce = 0;
        if (!find_json_container_range(raw, root_s, root_e, name.c_str(), cs, ce, true)) continue;
        bool touched = false;
        size_t pos = cs;
        while (pos < ce) {
            size_t hit = raw.find("rate_limit_us", pos);
            if (hit == std::string::npos || hit >= ce) break;
            pos = hit + 13;

            bool is_up = (hit >= cs + 3 && raw.compare(hit - 3, 3, "up_") == 0);
            bool is_dn = (hit >= cs + 5 && raw.compare(hit - 5, 5, "down_") == 0);
            if (!is_up && !is_dn) continue;

            size_t back_start = (hit > cs + 96) ? (hit - 96) : cs;
            int cpu_num = extract_cpu_num_from_path(raw.substr(back_start, hit - back_start));
            if (cpu_num < 0) continue;

            int c_idx = -1;
            for (size_t i = 0; i < clusters.size(); ++i) {
                if (cluster_owns_cpu(clusters[i], cpu_num)) { c_idx = static_cast<int>(i); break; }
            }
            if (c_idx < 0) continue;

            size_t q1 = raw.find('"', hit);
            if (q1 == std::string::npos || q1 >= ce) continue;
            size_t q2 = raw.find('"', q1 + 1);
            if (q2 == std::string::npos || q2 >= ce) continue;
            // Verify only separators/whitespace appear between q1 and q2
            bool sep_ok = true;
            for (size_t k = q1 + 1; k < q2; ++k) {
                char c = raw[k];
                if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != ':' && c != ',') {
                    sep_ok = false;
                    break;
                }
            }
            if (!sep_ok) continue;
            size_t q3 = raw.find('"', q2 + 1);
            if (q3 == std::string::npos || q3 >= ce || q3 - q2 > 24) continue;
            std::string val_s = trim(raw.substr(q2 + 1, q3 - q2 - 1));
            if (val_s.empty() || !std::all_of(val_s.begin(), val_s.end(),
                                              [](unsigned char ch) { return ch >= '0' && ch <= '9'; })) {
                continue;
            }
            uint64_t want = is_up ? cfg.up[c_idx] : cfg.dn[c_idx];
            patches.push_back({q2 + 1, q3, std::to_string(want)});
            touched = true;
        }
        if (touched) updated_schemes.push_back(name);
    }

    if (patches.empty()) return false;
    std::sort(patches.begin(), patches.end(), [](const JsonPatch& a, const JsonPatch& b) { return a.start > b.start; });
    for (const auto& p : patches) {
        raw.replace(p.start, p.end - p.start, p.repl);
    }
    if (!write_atomic_preserve(paths.scene_profile, raw, 0600)) return false;

    for (const auto& name : updated_schemes) {
        std::string obj_path = paths.scene_obj_dir + "/" + name;
        if (file_exists(obj_path)) {
            patch_scene_object_bin(obj_path, clusters, cfg);
        }
        if (!synced_out.empty()) synced_out += ",";
        synced_out += name;
    }
    return true;
}

std::string normalize_mode(const std::string& v) {
    if (v == "override" || v == "shield" || v == "compat") return v;
    if (v == "scene") return "shield";
    if (v == "lock" || v == "smart" || v == "passive") return "override";
    return "override";
}

void load_config(const Paths& paths, const std::vector<Cluster>& clusters, Config& cfg) {
    // Seed defaults from Scene if config file does not exist yet
    if (!file_exists(paths.conf_file)) {
        SceneRates sr = parse_scene_profile(paths, clusters);
        if (sr.found) {
            for (size_t i = 0; i < clusters.size(); ++i) {
                if (sr.has_up[i]) cfg.up[i] = sr.up[i];
                if (sr.has_dn[i]) cfg.dn[i] = sr.dn[i];
            }
        }
        return;
    }

    std::string raw;
    if (!read_file_str(paths.conf_file, raw, 16384)) return;
    std::istringstream iss(raw);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1));
        if (k == "enabled") cfg.enabled = (v == "1" || v == "true");
        else if (k == "mode") cfg.mode = normalize_mode(v);
        else if (k == "low_bat_threshold") {
            int t = std::atoi(v.c_str());
            if (t >= 1 && t <= 50) cfg.low_bat_threshold = t;
        }
        else if (k == "watch_charger") cfg.watch_charger = (v == "1" || v == "true");
        else if (k == "watch_screen") cfg.watch_screen = (v == "1" || v == "true");
        else if (k.size() == 5 && k[0] == 'c' && k[1] >= '0' && k[1] < '0' + kMaxClusters) {
            int idx = k[1] - '0';
            uint64_t val = strtoull(v.c_str(), nullptr, 10);
            if (val <= kMaxRateUs) {
                if (k.substr(2) == "_up") cfg.up[idx] = val;
                else if (k.substr(2) == "_dn") cfg.dn[idx] = val;
            }
        }
    }
}

bool save_config(const Paths& paths, const Config& cfg) {
    ensure_dir(paths.data_dir, 0700);
    std::ostringstream oss;
    oss << "enabled=" << (cfg.enabled ? 1 : 0) << "\n"
        << "mode=" << cfg.mode << "\n"
        << "low_bat_threshold=" << cfg.low_bat_threshold << "\n"
        << "watch_charger=" << (cfg.watch_charger ? 1 : 0) << "\n"
        << "watch_screen=" << (cfg.watch_screen ? 1 : 0) << "\n";
    for (int i = 0; i < kMaxClusters; ++i) {
        oss << "c" << i << "_up=" << cfg.up[i] << "\n"
            << "c" << i << "_dn=" << cfg.dn[i] << "\n";
    }
    return write_atomic_file(paths.conf_file, oss.str(), 0600);
}

void load_state(const Paths& paths, State& st) {
    std::string raw;
    if (!read_file_str(paths.state_file, raw, 4096)) return;
    std::istringstream iss(raw);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim(line);
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1));
        if (k == "fixes") st.fixes = strtoull(v.c_str(), nullptr, 10);
        else if (k == "last_fix_ts") st.last_fix_ts = strtoll(v.c_str(), nullptr, 10);
        else if (k == "last_reason") st.last_reason = v;
    }
}

void save_state(const Paths& paths, State& st) {
    ensure_dir(paths.data_dir, 0700);
    std::ostringstream oss;
    oss << "fixes=" << st.fixes << "\n"
        << "last_fix_ts=" << st.last_fix_ts << "\n"
        << "last_reason=" << st.last_reason << "\n";
    if (write_atomic_file(paths.state_file, oss.str(), 0600)) {
        st.dirty = false;
        st.last_flush_ms = mono_ms();
    }
}

struct PowerInfo {
    bool charging = false;
    int battery = -1;
    std::map<std::string, uint64_t> online_map;
};

PowerInfo read_power_info(const Paths& paths) {
    PowerInfo pi;
    DIR* d = ::opendir(paths.pwr_root.c_str());
    if (!d) return pi;
    struct dirent* ent = nullptr;
    while ((ent = ::readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string name = ent->d_name;
        std::string dir = paths.pwr_root + "/" + name;

        if (name == "battery" || name == "Battery" || name == "bms") {
            uint64_t cap = 0;
            if (pi.battery < 0 && read_uint(dir + "/capacity", cap)) {
                pi.battery = static_cast<int>(cap);
            }
            std::string status;
            if (read_file_str(dir + "/status", status, 32)) {
                status = trim(status);
                if (status == "Charging" || status == "Full") pi.charging = true;
            }
            continue;
        }
        if (name.find("gauge") != std::string::npos || name.find("bms") != std::string::npos) continue;
        uint64_t online = 0;
        if (read_uint(dir + "/online", online)) {
            pi.online_map[name] = online;
            if (online > 0) pi.charging = true;
        }
    }
    ::closedir(d);
    return pi;
}

bool is_daemon_running(const Paths& paths, pid_t* out_pid = nullptr) {
    UniqueFd fd(::open(paths.lock_file.c_str(), O_RDONLY | O_CLOEXEC));
    if (fd.valid()) {
        if (::flock(fd.get(), LOCK_EX | LOCK_NB) == 0) {
            ::flock(fd.get(), LOCK_UN);
            return false;
        }
        if (errno == EWOULDBLOCK) {
            if (out_pid) {
                std::string s;
                if (read_file_str(paths.pid_file, s, 32)) *out_pid = std::atoi(trim(s).c_str());
            }
            return true;
        }
    }
    std::string s;
    if (!read_file_str(paths.pid_file, s, 32)) return false;
    int pid = std::atoi(trim(s).c_str());
    if (pid <= 1) return false;
    std::string cmdline;
    if (!read_file_str("/proc/" + std::to_string(pid) + "/cmdline", cmdline, 256)) return false;
    if (cmdline.find("chaoxid") != std::string::npos) {
        if (out_pid) *out_pid = pid;
        return true;
    }
    return false;
}

void notify_daemon(const Paths& paths) {
    pid_t pid = 0;
    if (is_daemon_running(paths, &pid) && pid > 1) {
        ::kill(pid, SIGUSR1);
    }
}

bool is_governor_rate_active(const std::string& gov) {
    // Only touch sugov_ext/schedutil-like governors; ignore powersave/performance
    return gov != "powersave" && gov != "performance" && gov != "conservative";
}

bool write_str_quiet(const char* path, const std::string& content) {
    UniqueFd fd(::open(path, O_WRONLY | O_CLOEXEC));
    if (!fd.valid()) return false;
    ssize_t n = ::write(fd.get(), content.data(), content.size());
    return n > 0;
}

void protect_daemon_process() {
    if (::geteuid() != 0) return;
    std::string pid_s = std::to_string(::getpid()) + "\n";
    // Move out of caller app cgroup (e.g. KsuWebUI memcg/mimd) so LMK/freezer never kills daemon
    write_str_quiet("/dev/memcg/cgroup.procs", pid_s);
    write_str_quiet("/dev/cpuctl/cgroup.procs", pid_s);
    write_str_quiet("/dev/cpuset/cgroup.procs", pid_s);
    write_str_quiet("/dev/blkio/cgroup.procs", pid_s);
    std::string v2_dir = "/sys/fs/cgroup/uid_0/pid_" + std::to_string(::getpid());
    ::mkdir("/sys/fs/cgroup/uid_0", 0755);
    ::mkdir(v2_dir.c_str(), 0755);
    if (!write_str_quiet((v2_dir + "/cgroup.procs").c_str(), pid_s)) {
        write_str_quiet("/sys/fs/cgroup/cgroup.procs", pid_s);
    }
    write_str_quiet("/proc/self/oom_score_adj", "-900\n");
}

void detach_daemon_stdio() {
    ::setsid();
    ::signal(SIGHUP, SIG_IGN);
    ::signal(SIGPIPE, SIG_IGN);
    UniqueFd devnull(::open("/dev/null", O_RDWR | O_CLOEXEC));
    if (devnull.valid()) {
        ::dup2(devnull.get(), STDIN_FILENO);
        ::dup2(devnull.get(), STDOUT_FILENO);
        ::dup2(devnull.get(), STDERR_FILENO);
    }
}

bool set_cluster_shield(const Cluster& c, bool readonly) {
    if (!is_governor_rate_active(c.governor) || c.up_path.empty() || c.dn_path.empty()) return false;
    mode_t target = readonly ? 0444 : 0664;
    bool changed = false;
    for (const std::string* p : {&c.up_path, &c.dn_path}) {
        struct stat st {};
        if (::stat(p->c_str(), &st) == 0 && (st.st_mode & 0777) != target) {
            if (::chmod(p->c_str(), target) == 0) changed = true;
        }
    }
    return changed;
}

bool sync_all_shields(const Config& cfg, std::vector<Cluster>& clusters, bool force_unshield = false) {
    bool want_shield = !force_unshield && cfg.enabled && mode_uses_shield(cfg.mode);
    bool changed = false;
    for (auto& c : clusters) {
        refresh_cluster_rate_paths(c);
        if (set_cluster_shield(c, want_shield)) changed = true;
    }
    return changed;
}

bool enforce_clusters(const Config& cfg, std::vector<Cluster>& clusters,
                      State& st, Logger& log, const std::string& reason,
                      bool force, bool count_as_fix = true,
                      bool only_fix_reset = false, bool* out_wrote_any = nullptr) {
    if (out_wrote_any) *out_wrote_any = false;
    if (!cfg.enabled) return true;
    bool all_ok = true;
    int64_t now_s = static_cast<int64_t>(time(nullptr));
    int64_t now_m = mono_ms();
    bool want_shield = mode_uses_shield(cfg.mode);
    bool skip_active_boost = (!force && cfg.mode == "compat" &&
                              reason != "screen_off" && reason != "charger_unplug" &&
                              reason != "low_battery" && reason != "boot");

    for (auto& c : clusters) {
        refresh_cluster_rate_paths(c);
        if (!is_governor_rate_active(c.governor)) continue;
        if (c.up_path.empty() || c.dn_path.empty()) continue;

        if (!force && now_m < c.suppress_until_ms) {
            if (want_shield) set_cluster_shield(c, true);
            continue;
        }

        uint64_t want_up = cfg.up[c.index];
        uint64_t want_dn = cfg.dn[c.index];
        uint64_t cur_up = 0, cur_dn = 0;
        bool got_up = read_uint(c.up_path, cur_up);
        bool got_dn = read_uint(c.dn_path, cur_dn);

        // In shield mode, preserve valid Scene per-app values and only repair HAL/kernel resets (<=100us)
        if (only_fix_reset && !force) {
            bool up_corrupted = !got_up || (cur_up <= 100 && want_up > 100);
            bool dn_corrupted = !got_dn || (cur_dn <= 100 && want_dn > 100);
            if (!up_corrupted && !dn_corrupted) {
                if (want_shield) set_cluster_shield(c, true);
                continue;
            }
        }

        // In compat mode, exempt active vendor boosts (up=0) to avoid fighting PowerHAL
        if (skip_active_boost && got_up && cur_up == 0 && want_up > 0) {
            continue;
        }

        bool need_up = force || !got_up || cur_up != want_up;
        bool need_dn = force || !got_dn || cur_dn != want_dn;
        if (!need_up && !need_dn) {
            if (want_shield) set_cluster_shield(c, true);
            continue;
        }

        bool changed = false;
        if (need_up) {
            if (write_uint_sysfs(c.up_path, want_up)) {
                if (out_wrote_any) *out_wrote_any = true;
                if (!got_up || cur_up != want_up) changed = true;
            } else {
                all_ok = false;
            }
        }
        if (need_dn) {
            if (write_uint_sysfs(c.dn_path, want_dn)) {
                if (out_wrote_any) *out_wrote_any = true;
                if (!got_dn || cur_dn != want_dn) changed = true;
            } else {
                all_ok = false;
            }
        }
        if (want_shield) set_cluster_shield(c, true);

        if (changed) {
            if (count_as_fix) {
                ++st.fixes;
                st.last_fix_ts = now_s;
                st.last_reason = reason;
                st.dirty = true;
                if (!force) {
                    if (now_m - c.burst_start_ms > 4000) {
                        c.burst_start_ms = now_m;
                        c.burst_count = 1;
                    } else {
                        ++c.burst_count;
                        if (c.burst_count > 4) {
                            c.suppress_until_ms = now_m + 10000;
                            c.burst_count = 0;
                            log.write("cooldown 10s on " + c.policy_name + " (contention)");
                        }
                    }
                }
            }
            std::string entry = std::string(count_as_fix ? "fix " : "apply ") +
                                c.policy_name + " (" + reason + ") " +
                                (got_up ? std::to_string(cur_up) : "?") + "/" +
                                (got_dn ? std::to_string(cur_dn) : "?") + " -> " +
                                std::to_string(want_up) + "/" + std::to_string(want_dn);
            if (count_as_fix && reason == "rate_check") log.write_fix_throttled(entry);
            else log.write(entry);
        }
    }
    return all_ok;
}

class Daemon {
public:
    Daemon(Paths p) : paths_(std::move(p)) {
        log_.path = paths_.log_file;
    }

    int run() {
        ensure_dir(paths_.data_dir, 0700);
        lock_fd_.reset(::open(paths_.lock_file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
        if (!lock_fd_.valid()) return 1;
        if (::flock(lock_fd_.get(), LOCK_EX | LOCK_NB) != 0) {
            return 0;
        }

        detach_daemon_stdio();
        protect_daemon_process();

        write_atomic_file(paths_.pid_file, std::to_string(::getpid()) + "\n", 0644);

        for (int i = 0; i < 60; ++i) {
            if (get_prop("sys.boot_completed") == "1") break;
            ::sleep(1);
        }

        ep_fd_.reset(::epoll_create1(EPOLL_CLOEXEC));
        ino_fd_.reset(::inotify_init1(IN_NONBLOCK | IN_CLOEXEC));
        timer_fd_.reset(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
        if (!ep_fd_.valid() || !ino_fd_.valid() || !timer_fd_.valid()) return 1;

        setup_signals();
        setup_uevent();
        add_epoll(ino_fd_.get(), EPOLLIN);
        add_epoll(timer_fd_.get(), EPOLLIN);
        if (uevent_fd_.valid()) add_epoll(uevent_fd_.get(), EPOLLIN);
        if (sig_fd_.valid()) add_epoll(sig_fd_.get(), EPOLLIN);

        clusters_ = discover_clusters(paths_);
        load_config(paths_, clusters_, cfg_);
        if (!file_exists(paths_.conf_file)) save_config(paths_, cfg_);
        load_state(paths_, state_);

        init_local_tz_once();

        PowerInfo pi = read_power_info(paths_);
        last_charging_ = pi.charging;
        last_battery_ = pi.battery;
        low_bat_last_reported_ = pi.battery;
        psy_online_ = std::move(pi.online_map);
        backlight_path_ = find_backlight();
        if (!backlight_path_.empty()) {
            uint64_t bl = 0;
            if (read_uint(backlight_path_, bl)) prev_bl_ = bl;
        }

        sync_shields_quiet(false);
        arm_watches();

        log_.write("daemon started pid=" + std::to_string(::getpid()) +
                   " clusters=" + std::to_string(clusters_.size()) +
                   " mode=" + cfg_.mode +
                   " shield=" + (cfg_.enabled && mode_uses_shield(cfg_.mode) ? "0444" : "0664"));

        if (cfg_.enabled) {
            run_enforce(cfg_, "boot", false, false, cfg_.mode == "shield");
            schedule_burst({3000}, "boot_verify");
        }

        struct epoll_event events[16];
        while (!stop_requested_) {
            int n = ::epoll_wait(ep_fd_.get(), events, 16, -1);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            for (int i = 0; i < n; ++i) {
                int fd = events[i].data.fd;
                if (fd == ino_fd_.get()) handle_inotify();
                else if (fd == uevent_fd_.get()) handle_uevent();
                else if (fd == timer_fd_.get()) handle_timer();
                else if (fd == sig_fd_.get()) handle_signal();
            }
            flush_state_throttled(false);
        }

        // Cleanly restore 0664 permissions on daemon stop so system/user is never locked out
        sync_all_shields(cfg_, clusters_, true);
        flush_state_throttled(true);
        log_.write("daemon stopped (0664 restored)");
        ::unlink(paths_.pid_file.c_str());
        return 0;
    }

private:
    Paths paths_;
    Logger log_;
    Config cfg_;
    State state_;
    std::vector<Cluster> clusters_;

    UniqueFd lock_fd_;
    UniqueFd ep_fd_;
    UniqueFd ino_fd_;
    UniqueFd timer_fd_;
    UniqueFd uevent_fd_;
    UniqueFd sig_fd_;

    int wd_data_dir_ = -1;
    int wd_bl_ = -1;
    std::string backlight_path_;
    uint64_t prev_bl_ = 1;

    bool last_charging_ = false;
    int last_battery_ = -1;
    std::map<std::string, uint64_t> psy_online_;

    struct PendingCheck {
        int64_t at_ms;
        std::string reason;
    };
    std::vector<PendingCheck> timers_;
    bool stop_requested_ = false;

    void add_epoll(int fd, uint32_t ev) {
        struct epoll_event e {};
        e.events = ev;
        e.data.fd = fd;
        ::epoll_ctl(ep_fd_.get(), EPOLL_CTL_ADD, fd, &e);
    }

    void setup_signals() {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGTERM);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGUSR1);
        ::sigprocmask(SIG_BLOCK, &mask, nullptr);
        sig_fd_.reset(::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC));
    }

    void setup_uevent() {
        int fd = ::socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
        if (fd < 0) return;
        int buf_sz = 256 * 1024;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_sz, sizeof(buf_sz));
        struct sockaddr_nl addr {};
        addr.nl_family = AF_NETLINK;
        addr.nl_pid = 0;
        addr.nl_groups = 1;
        if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd);
            return;
        }
        uevent_fd_.reset(fd);
    }

    std::string find_backlight() {
        const char* fixed = "/sys/class/leds/lcd-backlight/brightness";
        if (file_exists(fixed)) return fixed;
        DIR* d = ::opendir("/sys/class/backlight");
        if (d) {
            struct dirent* ent = nullptr;
            while ((ent = ::readdir(d)) != nullptr) {
                if (ent->d_name[0] == '.') continue;
                std::string p = std::string("/sys/class/backlight/") + ent->d_name + "/brightness";
                if (file_exists(p)) { ::closedir(d); return p; }
            }
            ::closedir(d);
        }
        return "";
    }

    void rm_watch(int& wd) {
        if (wd >= 0) {
            ::inotify_rm_watch(ino_fd_.get(), wd);
            wd = -1;
        }
    }

    void disarm_rate_watches() {
        for (auto& c : clusters_) {
            rm_watch(c.wd_up);
            rm_watch(c.wd_dn);
            c.wd_up_ino = 0;
            c.wd_dn_ino = 0;
            c.wd_up_mask = 0;
            c.wd_dn_mask = 0;
        }
    }

    void arm_cluster_watches(Cluster& c, int64_t now_m) {
        if (!cfg_.enabled) {
            rm_watch(c.wd_dir);
            rm_watch(c.wd_gov);
            rm_watch(c.wd_up);
            rm_watch(c.wd_dn);
            c.wd_up_ino = 0;
            c.wd_dn_ino = 0;
            c.wd_up_mask = 0;
            c.wd_dn_mask = 0;
            return;
        }
        if (c.wd_dir < 0) {
            c.wd_dir = ::inotify_add_watch(ino_fd_.get(), c.policy_dir.c_str(), IN_CREATE | IN_DELETE);
        }
        if (c.wd_gov < 0) {
            std::string gpath = c.policy_dir + "/scaling_governor";
            c.wd_gov = ::inotify_add_watch(ino_fd_.get(), gpath.c_str(), IN_CLOSE_WRITE | IN_MODIFY);
        }
        refresh_cluster_rate_paths(c);
        // shield mode: watch IN_ATTRIB only -> 0 wakeups from blocked PowerHAL, instant 0444 re-lock if Scene chmods 0664!
        // override mode: watch writes + IN_ATTRIB -> catches CAP_DAC_OVERRIDE writes & chmods
        // compat mode: watch writes only (0664 boost-exempt soft guard)
        bool need_rate_watch = (cfg_.mode == "shield") || (now_m >= mute_rates_until_ms_);
        if (!need_rate_watch) {
            rm_watch(c.wd_up);
            rm_watch(c.wd_dn);
            c.wd_up_ino = 0;
            c.wd_dn_ino = 0;
            c.wd_up_mask = 0;
            c.wd_dn_mask = 0;
            return;
        }
        uint32_t rate_mask = (cfg_.mode == "shield")
                                 ? (IN_ATTRIB | IN_DELETE_SELF)
                                 : (cfg_.mode == "override"
                                        ? (IN_CLOSE_WRITE | IN_MODIFY | IN_ATTRIB | IN_DELETE_SELF)
                                        : (IN_CLOSE_WRITE | IN_MODIFY | IN_DELETE_SELF));
        struct stat su {};
        if (!c.up_path.empty() && ::stat(c.up_path.c_str(), &su) == 0) {
            if (c.wd_up < 0 || su.st_ino != c.wd_up_ino || c.wd_up_mask != rate_mask) {
                rm_watch(c.wd_up);
                c.wd_up = ::inotify_add_watch(ino_fd_.get(), c.up_path.c_str(), rate_mask);
                c.wd_up_ino = (c.wd_up >= 0) ? su.st_ino : 0;
                c.wd_up_mask = (c.wd_up >= 0) ? rate_mask : 0;
            }
        } else {
            rm_watch(c.wd_up);
            c.wd_up_ino = 0;
            c.wd_up_mask = 0;
        }
        struct stat sd {};
        if (!c.dn_path.empty() && ::stat(c.dn_path.c_str(), &sd) == 0) {
            if (c.wd_dn < 0 || sd.st_ino != c.wd_dn_ino || c.wd_dn_mask != rate_mask) {
                rm_watch(c.wd_dn);
                c.wd_dn = ::inotify_add_watch(ino_fd_.get(), c.dn_path.c_str(), rate_mask);
                c.wd_dn_ino = (c.wd_dn >= 0) ? sd.st_ino : 0;
                c.wd_dn_mask = (c.wd_dn >= 0) ? rate_mask : 0;
            }
        } else {
            rm_watch(c.wd_dn);
            c.wd_dn_ino = 0;
            c.wd_dn_mask = 0;
        }
    }

    void arm_watches() {
        int64_t now_m = mono_ms();
        if (wd_data_dir_ < 0) {
            wd_data_dir_ = ::inotify_add_watch(ino_fd_.get(), paths_.data_dir.c_str(),
                                               IN_CLOSE_WRITE | IN_MOVED_TO);
        }
        if (cfg_.enabled && cfg_.watch_screen && !backlight_path_.empty()) {
            if (wd_bl_ < 0 && now_m >= bl_rearm_block_until_ms_) {
                wd_bl_ = ::inotify_add_watch(ino_fd_.get(), backlight_path_.c_str(),
                                             IN_CLOSE_WRITE | IN_MODIFY);
            }
        } else {
            rm_watch(wd_bl_);
        }
        for (auto& c : clusters_) arm_cluster_watches(c, now_m);
    }

    void rearm_timerfd() {
        struct itimerspec its {};
        if (!timers_.empty()) {
            int64_t next_ms = timers_.front().at_ms;
            for (const auto& t : timers_) {
                if (t.at_ms < next_ms) next_ms = t.at_ms;
            }
            int64_t delta = next_ms - mono_ms();
            if (delta < 1) delta = 1;
            its.it_value.tv_sec = delta / 1000;
            its.it_value.tv_nsec = (delta % 1000) * 1000000L;
        }
        ::timerfd_settime(timer_fd_.get(), 0, &its, nullptr);
    }

    void schedule_burst(std::initializer_list<int> delays_ms, const std::string& reason) {
        int64_t now = mono_ms();
        timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                                     [&](const PendingCheck& p) { return p.reason == reason; }),
                      timers_.end());
        for (int d : delays_ms) {
            timers_.push_back({now + d, reason});
        }
        if (timers_.size() > 24) {
            std::sort(timers_.begin(), timers_.end(),
                      [](const PendingCheck& a, const PendingCheck& b) { return a.at_ms < b.at_ms; });
            timers_.resize(24);
        }
        rearm_timerfd();
    }

    void sync_shields_quiet(bool force_unshield = false) {
        sync_all_shields(cfg_, clusters_, force_unshield);
    }

    void run_enforce(const Config& target, const std::string& reason,
                     bool force, bool count_as_fix = true, bool only_fix_reset = false) {
        bool wrote = false;
        enforce_clusters(target, clusters_, state_, log_, reason, force, count_as_fix, only_fix_reset, &wrote);
        if (wrote) {
            // Suppress self-triggered inotify echoes from our own sysfs writes
            suppress_rate_until_ms_ = mono_ms() + 100;
        }
    }

    void schedule_rate_check(int delay_ms) {
        int64_t now = mono_ms();
        for (auto& t : timers_) {
            if (t.reason == "rate_check") {
                int64_t max_at = rate_check_first_ms_ + 360;
                int64_t want_at = std::min(max_at, now + delay_ms);
                if (want_at > t.at_ms) {
                    t.at_ms = want_at;
                    rearm_timerfd();
                }
                return;
            }
        }
        rate_check_first_ms_ = now;
        timers_.push_back({now + delay_ms, "rate_check"});
        rearm_timerfd();
    }

    void reload_config_and_apply(const std::string& reason) {
        Config prev = cfg_;
        load_config(paths_, clusters_, cfg_);
        for (auto& c : clusters_) {
            c.suppress_until_ms = 0;
            c.burst_count = 0;
        }
        mute_rates_until_ms_ = 0;
        rate_burst_count_ = 0;
        attrib_burst_count_ = 0;
        attrib_mute_until_ms_ = 0;
        suppress_rate_until_ms_ = mono_ms() + 180;
        timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                                     [](const PendingCheck& p) {
                                         return p.reason == "rate_check" || p.reason == "rate_rearm" ||
                                                p.reason == "shield_relock";
                                     }),
                      timers_.end());
        rearm_timerfd();

        sync_shields_quiet(false);
        arm_watches();

        if (cfg_.enabled) {
            if (prev.mode != cfg_.mode || !prev.enabled) {
                log_.write("mode=" + cfg_.mode +
                           " shield=" + (mode_uses_shield(cfg_.mode) ? "0444" : "0664"));
            }
            run_enforce(cfg_, reason, false, false, cfg_.mode == "shield");
        } else if (prev.enabled && !cfg_.enabled) {
            timers_.clear();
            rearm_timerfd();
            log_.write("paused by config (0664 restored)");
        }
    }

    void handle_inotify() {
        alignas(struct inotify_event) char buf[8192];
        bool config_dirty = false;
        bool bl_dirty = false;
        bool need_rearm = false;
        bool gov_event = false;
        bool dir_recreated = false;
        bool rate_touched = false;
        bool attrib_unshielded = false;
        std::vector<int> touched_govs;

        int64_t now_m = mono_ms();
        bool ignore_rate_echo = (now_m < suppress_rate_until_ms_ || now_m < mute_rates_until_ms_);

        while (true) {
            ssize_t len = ::read(ino_fd_.get(), buf, sizeof(buf));
            if (len <= 0) break;
            ssize_t off = 0;
            while (off < len) {
                const auto* ev = reinterpret_cast<const struct inotify_event*>(buf + off);
                off += static_cast<ssize_t>(sizeof(struct inotify_event) + ev->len);
                std::string name = (ev->len > 0) ? std::string(ev->name) : std::string();

                if (ev->wd == wd_data_dir_) {
                    if (name == "config.conf") config_dirty = true;
                    continue;
                }
                if (ev->wd == wd_bl_) {
                    bl_dirty = true;
                    continue;
                }

                for (size_t i = 0; i < clusters_.size(); ++i) {
                    Cluster& c = clusters_[i];
                    if (ev->wd == c.wd_up || ev->wd == c.wd_dn) {
                        if (ev->mask & (IN_DELETE_SELF | IN_IGNORED)) {
                            if (ev->wd == c.wd_up) { c.wd_up = -1; c.wd_up_ino = 0; c.wd_up_mask = 0; }
                            if (ev->wd == c.wd_dn) { c.wd_dn = -1; c.wd_dn_ino = 0; c.wd_dn_mask = 0; }
                            need_rearm = true;
                        } else {
                            // Self chmod 0444 leaves is_path_shielded(p)==true so it never loops;
                            // external chmod 0664 (e.g. scene-daemon) leaves it false and gets re-locked <1ms!
                            if ((ev->mask & IN_ATTRIB) && mode_uses_shield(cfg_.mode) && now_m >= attrib_mute_until_ms_) {
                                const std::string& p = (ev->wd == c.wd_up) ? c.up_path : c.dn_path;
                                if (!p.empty() && !is_path_shielded(p)) attrib_unshielded = true;
                            }
                            if ((ev->mask & (IN_CLOSE_WRITE | IN_MODIFY)) && !ignore_rate_echo) {
                                rate_touched = true;
                            }
                        }
                    } else if (ev->wd == c.wd_gov) {
                        if (ev->mask & IN_IGNORED) c.wd_gov = -1;
                        need_rearm = true;
                        touched_govs.push_back(static_cast<int>(i));
                    } else if (ev->wd == c.wd_dir) {
                        if (name == "sugov_ext" || name == "schedutil" || name == "scaling_governor") {
                            need_rearm = true;
                            if (ev->mask & IN_CREATE) dir_recreated = true;
                            touched_govs.push_back(static_cast<int>(i));
                        }
                    }
                }
            }
        }

        if (config_dirty) {
            reload_config_and_apply("config_change");
            return;
        }

        std::vector<std::string> prev_govs;
        if (need_rearm) {
            prev_govs.reserve(clusters_.size());
            for (const auto& c : clusters_) prev_govs.push_back(c.governor);
            arm_watches();
            if (!touched_govs.empty()) {
                for (int idx : touched_govs) {
                    if (dir_recreated || clusters_[idx].governor != prev_govs[idx]) {
                        gov_event = true;
                        break;
                    }
                }
            }
        }
        if (gov_event && cfg_.enabled) {
            // Re-lock 0444 immediately (<1ms) when kernel recreates sugov_ext/ at 0644
            if (mode_uses_shield(cfg_.mode)) {
                sync_shields_quiet(false);
            }
            schedule_burst({80, 350}, "governor_restore");
        }

        if (attrib_unshielded && cfg_.enabled && mode_uses_shield(cfg_.mode)) {
            if (now_m - attrib_burst_window_ms_ > 2000) {
                attrib_burst_window_ms_ = now_m;
                attrib_burst_count_ = 1;
            } else {
                ++attrib_burst_count_;
            }
            if (attrib_burst_count_ > 24) {
                attrib_mute_until_ms_ = now_m + 2000;
                attrib_burst_count_ = 0;
                schedule_burst({2050}, "shield_relock");
            } else {
                // Re-lock 0444 immediately (<1ms; CAP_DAC_OVERRIDE writers like scene-daemon still succeed!)
                sync_shields_quiet(false);
                schedule_burst({40}, "shield_relock");
            }
        }

        if (bl_dirty && cfg_.watch_screen) {
            uint64_t cur_bl = 0;
            if (read_uint(backlight_path_, cur_bl)) {
                if (prev_bl_ > 0 && cur_bl == 0) {
                    prev_bl_ = 0;
                    last_screen_off_ms_ = now_m;
                    if (cfg_.enabled && !mode_uses_shield(cfg_.mode)) {
                        schedule_burst({700}, "screen_off");
                    }
                } else if (prev_bl_ == 0 && cur_bl > 0) {
                    int64_t off_dur = (last_screen_off_ms_ > 0) ? (now_m - last_screen_off_ms_) : 999999;
                    prev_bl_ = cur_bl;
                    rm_watch(wd_bl_);
                    bl_rearm_block_until_ms_ = now_m + 8000;
                    schedule_burst({8050}, "bl_rearm");
                    if (cfg_.enabled && off_dur >= 2000) {
                        schedule_burst({1000}, "screen_on");
                    }
                } else if (prev_bl_ > 0 && cur_bl > 0) {
                    // Auto-brightness step while screen ON: back off 8s to avoid pointless wakeups
                    prev_bl_ = cur_bl;
                    rm_watch(wd_bl_);
                    bl_rearm_block_until_ms_ = now_m + 8000;
                    schedule_burst({8050}, "bl_rearm");
                } else {
                    prev_bl_ = cur_bl;
                }
            }
        }

        if (rate_touched && cfg_.enabled && cfg_.mode != "shield") {
            if (now_m - last_rate_burst_hit_ms_ >= 350) {
                last_rate_burst_hit_ms_ = now_m;
                if (now_m - rate_burst_window_ms_ > 2500) {
                    rate_burst_window_ms_ = now_m;
                    rate_burst_count_ = 1;
                } else {
                    ++rate_burst_count_;
                }
            }

            if (cfg_.mode == "override") {
                if (rate_burst_count_ > 5) {
                    mute_rates_until_ms_ = now_m + 5000;
                    rate_burst_count_ = 0;
                    disarm_rate_watches();
                    schedule_burst({5050}, "rate_rearm");
                    log_.write("override contention cooldown 5s");
                } else {
                    schedule_rate_check(80);
                }
            } else {
                // compat mode: coalesce 220ms and absorb sustained boost storms
                if (rate_burst_count_ > 3) {
                    mute_rates_until_ms_ = now_m + 2200;
                    rate_burst_count_ = 0;
                    disarm_rate_watches();
                    schedule_burst({2250}, "rate_rearm");
                } else {
                    schedule_rate_check(220);
                }
            }
        }
    }

    void handle_uevent() {
        char buf[8192];
        static constexpr char kPsyNeedle[] = "SUBSYSTEM=power_supply";
        while (true) {
            ssize_t len = ::recv(uevent_fd_.get(), buf, sizeof(buf), 0);
            if (len <= 0) break;
            if (!cfg_.enabled) continue;
            if (std::search(buf, buf + len, kPsyNeedle, kPsyNeedle + sizeof(kPsyNeedle) - 1) == buf + len) {
                continue;
            }

            std::string psy_name;
            std::string psy_status;
            int online = -1;
            int capacity = -1;

            ssize_t off = 0;
            while (off < len) {
                const char* s = buf + off;
                size_t slen = strnlen(s, static_cast<size_t>(len - off));
                if (std::strncmp(s, "POWER_SUPPLY_NAME=", 18) == 0) psy_name = s + 18;
                else if (std::strncmp(s, "POWER_SUPPLY_STATUS=", 20) == 0) psy_status = s + 20;
                else if (std::strncmp(s, "POWER_SUPPLY_ONLINE=", 20) == 0) online = std::atoi(s + 20);
                else if (std::strncmp(s, "POWER_SUPPLY_CAPACITY=", 22) == 0) capacity = std::atoi(s + 22);
                off += static_cast<ssize_t>(slen) + 1;
            }

            bool unplugged = false;
            bool plugged = false;
            bool is_batt = (psy_name == "battery" || psy_name == "Battery" || psy_name == "bms");

            if (!is_batt && online >= 0 && psy_name.find("gauge") == std::string::npos &&
                psy_name.find("bms") == std::string::npos) {
                auto it = psy_online_.find(psy_name);
                uint64_t prev_on = (it != psy_online_.end()) ? it->second : 0;
                if (prev_on > 0 && online == 0) unplugged = true;
                else if (prev_on == 0 && online > 0) plugged = true;
                psy_online_[psy_name] = static_cast<uint64_t>(online);
            }

            if (is_batt && !psy_status.empty()) {
                bool now_chg = (psy_status == "Charging" || psy_status == "Full");
                if (last_charging_ && !now_chg) unplugged = true;
                else if (!last_charging_ && now_chg) plugged = true;
                last_charging_ = now_chg;
            }

            int64_t now_m = mono_ms();
            bool shielded = mode_uses_shield(cfg_.mode);
            if (cfg_.watch_charger && unplugged) {
                if (now_m - last_unplug_event_ms_ >= 2000) {
                    last_unplug_event_ms_ = now_m;
                    if (shielded) schedule_burst({800}, "charger_unplug");
                    else schedule_burst({550, 2200}, "charger_unplug");
                }
            } else if (cfg_.watch_charger && plugged) {
                if (now_m - last_plug_event_ms_ >= 2000) {
                    last_plug_event_ms_ = now_m;
                    if (shielded) schedule_burst({800}, "charger_plug");
                    else schedule_burst({650, 2200}, "charger_plug");
                }
            }

            if (is_batt && capacity >= 0) {
                bool crossed_low = (capacity <= cfg_.low_bat_threshold) &&
                    (low_bat_last_reported_ < 0 || low_bat_last_reported_ > cfg_.low_bat_threshold ||
                     (low_bat_last_reported_ - capacity >= 5));
                if (crossed_low) {
                    low_bat_last_reported_ = capacity;
                    schedule_burst({600}, "low_battery");
                } else if (capacity > cfg_.low_bat_threshold) {
                    low_bat_last_reported_ = capacity;
                }
                last_battery_ = capacity;
            }
        }
    }

    void handle_timer() {
        uint64_t exp = 0;
        (void)::read(timer_fd_.get(), &exp, sizeof(exp));
        int64_t now = mono_ms();

        bool do_bl_rearm = false;
        bool do_rate_rearm = false;
        std::string enforce_reason;
        std::vector<PendingCheck> remaining;
        for (auto& t : timers_) {
            if (t.at_ms <= now + 5) {
                if (t.reason == "bl_rearm") do_bl_rearm = true;
                else if (t.reason == "rate_rearm") do_rate_rearm = true;
                else if (enforce_reason.empty()) enforce_reason = t.reason;
            } else {
                remaining.push_back(std::move(t));
            }
        }
        timers_ = std::move(remaining);
        rearm_timerfd();

        if (do_bl_rearm) {
            bl_rearm_block_until_ms_ = 0;
            if (!backlight_path_.empty()) {
                uint64_t cur_bl = 0;
                if (read_uint(backlight_path_, cur_bl)) {
                    if (prev_bl_ > 0 && cur_bl == 0) last_screen_off_ms_ = now;
                    prev_bl_ = cur_bl;
                }
            }
            arm_watches();
        }
        if (do_rate_rearm) {
            mute_rates_until_ms_ = 0;
            rate_burst_count_ = 0;
            arm_watches();
            if (enforce_reason.empty()) enforce_reason = "rate_rearm";
        }
        if (cfg_.enabled && !enforce_reason.empty()) {
            if (!do_bl_rearm && !do_rate_rearm) arm_watches();
            if (mode_uses_shield(cfg_.mode)) sync_shields_quiet(false);
            run_enforce(cfg_, enforce_reason, false, true, cfg_.mode == "shield");
        }
    }

    void handle_signal() {
        struct signalfd_siginfo si {};
        while (::read(sig_fd_.get(), &si, sizeof(si)) == sizeof(si)) {
            if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT) {
                stop_requested_ = true;
            } else if (si.ssi_signo == SIGUSR1) {
                reload_config_and_apply("signal_apply");
            }
        }
    }

    void flush_state_throttled(bool force) {
        if (!state_.dirty) return;
        if (force || (mono_ms() - state_.last_flush_ms >= 300000)) {
            save_state(paths_, state_);
        }
    }

private:
    int64_t bl_rearm_block_until_ms_ = 0;
    int64_t last_unplug_event_ms_ = 0;
    int64_t last_plug_event_ms_ = 0;
    int64_t last_screen_off_ms_ = 0;
    int64_t mute_rates_until_ms_ = 0;
    int64_t suppress_rate_until_ms_ = 0;
    int64_t attrib_burst_window_ms_ = 0;
    int64_t attrib_mute_until_ms_ = 0;
    int attrib_burst_count_ = 0;
    int64_t rate_check_first_ms_ = 0;
    int64_t rate_burst_window_ms_ = 0;
    int64_t last_rate_burst_hit_ms_ = 0;
    int rate_burst_count_ = 0;
    int low_bat_last_reported_ = -1;
};

void append_scene_scheme_json(std::ostringstream& oss, const SceneRates& sr, size_t nclusters) {
    if (!sr.found) {
        oss << "null";
        return;
    }
    oss << "[";
    for (size_t i = 0; i < nclusters; ++i) {
        if (i > 0) oss << ",";
        oss << "{\"up\":" << (sr.has_up[i] ? std::to_string(sr.up[i]) : "null")
            << ",\"dn\":" << (sr.has_dn[i] ? std::to_string(sr.dn[i]) : "null") << "}";
    }
    oss << "]";
}

void print_status_json(const Paths& paths) {
    std::vector<Cluster> clusters = discover_clusters(paths);
    Config cfg;
    load_config(paths, clusters, cfg);
    State st;
    load_state(paths, st);
    PowerInfo pi = read_power_info(paths);
    SceneRates sr = parse_scene_profile(paths, clusters);
    SceneAllSchemes schemes = parse_all_scene_schemes(paths, clusters);
    pid_t dpid = 0;
    bool running = is_daemon_running(paths, &dpid);

    int active_rate_clusters = 0;
    int shielded_clusters = 0;
    for (const auto& c : clusters) {
        if (is_governor_rate_active(c.governor) && !c.up_path.empty() && !c.dn_path.empty()) {
            ++active_rate_clusters;
            if (is_path_shielded(c.up_path) && is_path_shielded(c.dn_path)) ++shielded_clusters;
        }
    }
    bool shield_active = (active_rate_clusters > 0 && shielded_clusters == active_rate_clusters);

    std::ostringstream oss;
    oss << "{\"ok\":true"
        << ",\"enabled\":" << (cfg.enabled ? "true" : "false")
        << ",\"daemon\":" << (running ? "true" : "false")
        << ",\"daemon_pid\":" << dpid
        << ",\"mode\":\"" << json_escape(cfg.mode) << "\""
        << ",\"shield_active\":" << (shield_active ? "true" : "false")
        << ",\"low_bat_threshold\":" << cfg.low_bat_threshold
        << ",\"watch_charger\":" << (cfg.watch_charger ? "true" : "false")
        << ",\"watch_screen\":" << (cfg.watch_screen ? "true" : "false")
        << ",\"charging\":" << (pi.charging ? "true" : "false")
        << ",\"battery\":" << pi.battery
        << ",\"fixes\":" << st.fixes
        << ",\"last_fix_ts\":" << st.last_fix_ts
        << ",\"last_reason\":\"" << json_escape(st.last_reason) << "\""
        << ",\"scene_present\":" << (sr.found ? "true" : "false")
        << ",\"scene_schemes\":{\"powersave\":";
    append_scene_scheme_json(oss, schemes.powersave, clusters.size());
    oss << ",\"balance\":";
    append_scene_scheme_json(oss, schemes.balance, clusters.size());
    oss << ",\"performance\":";
    append_scene_scheme_json(oss, schemes.performance, clusters.size());
    oss << "}"
        << ",\"max_rate_us\":" << kMaxRateUs
        << ",\"clusters\":[";

    for (size_t i = 0; i < clusters.size(); ++i) {
        const auto& c = clusters[i];
        uint64_t cur_up = 0, cur_dn = 0;
        bool has_up = !c.up_path.empty() && read_uint(c.up_path, cur_up);
        bool has_dn = !c.dn_path.empty() && read_uint(c.dn_path, cur_dn);
        bool c_shield = !c.up_path.empty() && !c.dn_path.empty() &&
                        is_path_shielded(c.up_path) && is_path_shielded(c.dn_path);
        std::string nmode = !c.up_path.empty() ? format_path_mode(c.up_path) : "";
        if (i > 0) oss << ",";
        oss << "{\"index\":" << c.index
            << ",\"policy\":\"" << json_escape(c.policy_name) << "\""
            << ",\"cpus\":\"" << json_escape(c.cpus) << "\""
            << ",\"max_khz\":" << c.max_khz
            << ",\"governor\":\"" << json_escape(c.governor) << "\""
            << ",\"rate_dir\":\"" << json_escape(c.rate_dir) << "\""
            << ",\"shielded\":" << (c_shield ? "true" : "false")
            << ",\"node_mode\":\"" << json_escape(nmode) << "\""
            << ",\"cur_up\":" << (has_up ? std::to_string(cur_up) : "null")
            << ",\"cur_dn\":" << (has_dn ? std::to_string(cur_dn) : "null")
            << ",\"want_up\":" << cfg.up[i]
            << ",\"want_dn\":" << cfg.dn[i]
            << ",\"scene_up\":" << (sr.has_up[i] ? std::to_string(sr.up[i]) : "null")
            << ",\"scene_dn\":" << (sr.has_dn[i] ? std::to_string(sr.dn[i]) : "null")
            << "}";
    }
    oss << "]}\n";
    std::printf("%s", oss.str().c_str());
}

int cmd_apply(const Paths& paths, int argc, char** argv) {
    std::vector<Cluster> clusters = discover_clusters(paths);
    Config cfg;
    load_config(paths, clusters, cfg);
    State st;
    load_state(paths, st);
    Logger log{paths.log_file};

    if (clusters.empty()) {
        std::printf("{\"ok\":false,\"error\":\"no_clusters\"}\n");
        return 1;
    }

    std::string scheme = "none";
    std::vector<std::string> nums;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--scheme" && i + 1 < argc) {
            scheme = argv[++i];
        } else if (a.rfind("--scheme=", 0) == 0) {
            scheme = a.substr(9);
        } else {
            nums.push_back(std::move(a));
        }
    }

    int n = static_cast<int>(nums.size());
    int expected_pairs = static_cast<int>(clusters.size()) * 2;
    if (n != expected_pairs && n != static_cast<int>(clusters.size())) {
        std::printf("{\"ok\":false,\"error\":\"arg_count\",\"expected\":%d}\n", expected_pairs);
        return 1;
    }

    for (int i = 0; i < n; ++i) {
        const std::string& s = nums[i];
        if (s.empty()) { std::printf("{\"ok\":false,\"error\":\"empty_arg\"}\n"); return 1; }
        for (char c : s) {
            if (c < '0' || c > '9') { std::printf("{\"ok\":false,\"error\":\"invalid_number\"}\n"); return 1; }
        }
        uint64_t val = strtoull(s.c_str(), nullptr, 10);
        if (val > kMaxRateUs) { std::printf("{\"ok\":false,\"error\":\"out_of_range\"}\n"); return 1; }
        if (n == expected_pairs) {
            if (i % 2 == 0) cfg.up[i / 2] = val;
            else cfg.dn[i / 2] = val;
        } else {
            cfg.up[i] = val;
            cfg.dn[i] = val;
        }
    }

    cfg.enabled = true;
    if (!save_config(paths, cfg)) {
        std::printf("{\"ok\":false,\"error\":\"save_config_failed\"}\n");
        return 1;
    }

    std::string synced_out;
    if (scheme != "none") {
        if (!write_scene_schemes(paths, clusters, cfg, scheme, synced_out)) {
            std::printf("{\"ok\":false,\"error\":\"scene_sync_failed\"}\n");
            return 1;
        }
        log.write("sync default -> Scene [" + synced_out + "]");
    }

    bool ok = enforce_clusters(cfg, clusters, st, log, "webui_apply", true, false, false);
    sync_all_shields(cfg, clusters);
    save_state(paths, st);
    notify_daemon(paths);
    if (!ok) {
        std::printf("{\"ok\":false,\"error\":\"sysfs_write_partial\"}\n");
        return 1;
    }
    print_status_json(paths);
    return 0;
}

int cmd_set_config(const Paths& paths, const std::string& key, const std::string& val) {
    std::vector<Cluster> clusters = discover_clusters(paths);
    Config cfg;
    load_config(paths, clusters, cfg);
    if (key == "enabled") cfg.enabled = (val == "1" || val == "true");
    else if (key == "mode") cfg.mode = normalize_mode(val);
    else if (key == "low_bat_threshold") {
        int t = std::atoi(val.c_str());
        if (t < 1 || t > 50) { std::printf("{\"ok\":false,\"error\":\"invalid_threshold\"}\n"); return 1; }
        cfg.low_bat_threshold = t;
    }
    else if (key == "watch_charger") cfg.watch_charger = (val == "1" || val == "true");
    else if (key == "watch_screen") cfg.watch_screen = (val == "1" || val == "true");
    else {
        std::printf("{\"ok\":false,\"error\":\"unknown_key\"}\n");
        return 1;
    }
    if (!save_config(paths, cfg)) {
        std::printf("{\"ok\":false,\"error\":\"save_failed\"}\n");
        return 1;
    }
    sync_all_shields(cfg, clusters);
    if (cfg.enabled) {
        State st;
        load_state(paths, st);
        Logger log{paths.log_file};
        enforce_clusters(cfg, clusters, st, log, "set_" + key, false, false, cfg.mode == "shield");
        save_state(paths, st);
    }
    notify_daemon(paths);
    print_status_json(paths);
    return 0;
}

int cmd_sync_scene(const Paths& paths, const std::string& scheme) {
    std::vector<Cluster> clusters = discover_clusters(paths);
    const char* pref = (scheme == "powersave" || scheme == "balance" || scheme == "performance")
                           ? scheme.c_str()
                           : nullptr;
    SceneRates sr = parse_scene_profile(paths, clusters, pref);
    if (!sr.found) {
        std::printf("{\"ok\":false,\"error\":\"scene_profile_not_found\"}\n");
        return 1;
    }
    Config cfg;
    load_config(paths, clusters, cfg);
    for (size_t i = 0; i < clusters.size(); ++i) {
        if (sr.has_up[i]) cfg.up[i] = sr.up[i];
        if (sr.has_dn[i]) cfg.dn[i] = sr.dn[i];
    }
    cfg.enabled = true;
    save_config(paths, cfg);
    State st;
    load_state(paths, st);
    Logger log{paths.log_file};
    enforce_clusters(cfg, clusters, st, log, "sync_" + (pref ? std::string(pref) : std::string("scene")), true, false, false);
    sync_all_shields(cfg, clusters);
    save_state(paths, st);
    notify_daemon(paths);
    print_status_json(paths);
    return 0;
}

int cmd_log(const Paths& paths, int lines) {
    std::string raw;
    if (!read_file_str(paths.log_file, raw, kMaxLogBytes * 2) || raw.empty()) {
        std::printf("(日志为空)\n");
        return 0;
    }
    std::vector<std::string> all;
    std::istringstream iss(raw);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty()) all.push_back(line);
    }
    int start = (static_cast<int>(all.size()) > lines) ? (static_cast<int>(all.size()) - lines) : 0;
    for (int i = start; i < static_cast<int>(all.size()); ++i) {
        std::printf("%s\n", all[i].c_str());
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Paths paths;
    paths.init();
    ensure_dir(paths.data_dir, 0700);

    if (argc < 2) {
        std::fprintf(stderr, "usage: chaoxid {daemon|status|apply ...|set-config k v|sync-scene|log [N]|clear-log|stop}\n");
        return 2;
    }

    std::string cmd = argv[1];
    if (cmd == "daemon") {
        if (argc >= 3 && std::string(argv[2]) == "--bg") {
            pid_t p1 = ::fork();
            if (p1 < 0) return 1;
            if (p1 > 0) return 0;
            ::setsid();
            pid_t p2 = ::fork();
            if (p2 < 0) ::_exit(1);
            if (p2 > 0) ::_exit(0);
        }
        Daemon d(paths);
        return d.run();
    } else if (cmd == "status") {
        print_status_json(paths);
        return 0;
    } else if (cmd == "apply") {
        init_local_tz_once();
        return cmd_apply(paths, argc - 2, argv + 2);
    } else if (cmd == "set-config" && argc >= 4) {
        init_local_tz_once();
        return cmd_set_config(paths, argv[2], argv[3]);
    } else if (cmd == "sync-scene") {
        init_local_tz_once();
        std::string scheme = (argc >= 3) ? argv[2] : "balance";
        return cmd_sync_scene(paths, scheme);
    } else if (cmd == "log") {
        int n = (argc >= 3) ? std::max(1, std::min(500, std::atoi(argv[2]))) : 60;
        return cmd_log(paths, n);
    } else if (cmd == "clear-log") {
        write_atomic_file(paths.log_file, "", 0600);
        std::printf("{\"ok\":true}\n");
        return 0;
    } else if (cmd == "ui-get") {
        std::string raw;
        if (!read_file_str(paths.ui_file, raw, 2 * 1024 * 1024) || trim(raw).empty()) {
            std::printf("{}\n");
        } else {
            std::printf("%s\n", raw.c_str());
        }
        return 0;
    } else if (cmd == "ui-set") {
        std::string body;
        char buf[8192];
        while (body.size() < 2 * 1024 * 1024) {
            ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) break;
            body.append(buf, static_cast<size_t>(n));
        }
        if (write_atomic_file(paths.ui_file, body, 0600)) {
            std::printf("{\"ok\":true}\n");
            return 0;
        }
        std::printf("{\"ok\":false}\n");
        return 1;
    } else if (cmd == "stop") {
        pid_t pid = 0;
        if (is_daemon_running(paths, &pid) && pid > 1) ::kill(pid, SIGTERM);
        std::printf("{\"ok\":true}\n");
        return 0;
    }

    std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
    return 2;
}
