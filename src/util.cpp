#include "util.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <vector>

#include "dupes.h"

namespace fs = std::filesystem;

namespace {

// $VAR if it is set and non-empty, else $HOME/fallback, else the working directory.
std::string xdgDir(const char* var, const char* fallback) {
    fs::path base;
    if (const char* v = std::getenv(var); v && *v) {
        base = v;
    } else if (const char* h = std::getenv("HOME"); h && *h) {
        base = fs::path(h) / fallback;
    } else {
        base = ".";
    }
    return (base / "duplicates-ui").string();
}

// One side of the cross-device move: copy to a sibling of the destination,
// flush it, rename it into place, and only then remove the source.
bool copyThenReplace(const std::string& from, const std::string& to, std::string& err) {
    const int in = ::open(from.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        err = std::string("open source: ") + std::strerror(errno);
        return false;
    }
    struct stat st {};
    if (::fstat(in, &st) != 0) {
        err = std::string("stat source: ") + std::strerror(errno);
        ::close(in);
        return false;
    }

    const std::string tmp = to + ".dup-part";
    const int out = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                           st.st_mode & 07777);
    if (out < 0) {
        err = std::string("open destination: ") + std::strerror(errno);
        ::close(in);
        return false;
    }

    std::vector<char> buf(1u << 20);
    bool ok = true;
    for (;;) {
        const ssize_t n = ::read(in, buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            err = std::string("read: ") + std::strerror(errno);
            ok = false;
            break;
        }
        if (n == 0) break;
        ssize_t written = 0;
        while (written < n) {
            const ssize_t w = ::write(out, buf.data() + written, static_cast<size_t>(n - written));
            if (w < 0) {
                if (errno == EINTR) continue;
                err = std::string("write: ") + std::strerror(errno);
                ok = false;
                break;
            }
            written += w;
        }
        if (!ok) break;
    }

    if (ok) {
        struct timespec times[2] = {st.st_atim, st.st_mtim};
        ::futimens(out, times);
        if (::fsync(out) != 0) {
            err = std::string("fsync: ") + std::strerror(errno);
            ok = false;
        }
    }
    ::close(in);
    ::close(out);

    if (!ok || ::rename(tmp.c_str(), to.c_str()) != 0) {
        if (ok) err = std::string("rename into place: ") + std::strerror(errno);
        ::unlink(tmp.c_str());
        return false;
    }
    if (::unlink(from.c_str()) != 0) {
        err = std::string("copied, but the original could not be removed: ") +
              std::strerror(errno);
        return false;
    }
    return true;
}

}  // namespace

std::string stateDir() { return xdgDir("XDG_STATE_HOME", ".local/state"); }
std::string cacheDir() { return xdgDir("XDG_CACHE_HOME", ".cache"); }

std::string homeDir() {
    if (const char* h = std::getenv("HOME"); h && *h) return h;
    return "/";
}

bool ensureDir(const std::string& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
    if (!ec) return true;
    return fs::is_directory(path, ec);
}

std::string normalizePath(const std::string& raw) {
    const size_t b = raw.find_first_not_of(" \t\n\r");
    if (b == std::string::npos) return {};
    const size_t e = raw.find_last_not_of(" \t\n\r");
    std::string p = raw.substr(b, e - b + 1);

    if (p[0] == '~' && (p.size() == 1 || p[1] == '/')) p = homeDir() + p.substr(1);
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

bool moveFile(const std::string& from, const std::string& to, std::string& err) {
    if (::rename(from.c_str(), to.c_str()) == 0) return true;
    if (errno != EXDEV) {
        err = std::strerror(errno);
        return false;
    }
    return copyThenReplace(from, to, err);
}

bool pathIsUnder(const std::string& parent, const std::string& child) {
    if (parent.empty() || child.size() <= parent.size()) return false;
    if (child.compare(0, parent.size(), parent) != 0) return false;
    return parent == "/" || child[parent.size()] == '/';
}

std::string timestampNow() {
    const std::time_t now = std::time(nullptr);
    std::tm tm {};
    localtime_r(&now, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H-%M-%S", &tm);
    return buf;
}

std::string escapeField(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            default: out += c;
        }
    }
    return out;
}

std::string unescapeField(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out += s[i];
            continue;
        }
        switch (s[++i]) {
            case 't': out += '\t'; break;
            case 'n': out += '\n'; break;
            case '\\': out += '\\'; break;
            default: out += s[i];
        }
    }
    return out;
}

const char* const kTieBreakNames[5] = {"oldest mtime", "newest mtime", "shortest path",
                                       "fewest path segments", "alphabetical"};

const char* tieBreakName(TieBreak t) {
    const int i = static_cast<int>(t);
    return (i >= 0 && i < 5) ? kTieBreakNames[i] : kTieBreakNames[0];
}

std::string formatSize(uint64_t bytes) {
    static const char* kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double v = static_cast<double>(bytes);
    int unit = 0;
    while (v >= 1024.0 && unit < 5) {
        v /= 1024.0;
        ++unit;
    }
    char buf[48];
    // Bytes are always whole; anything larger reads better with one decimal.
    std::snprintf(buf, sizeof(buf), unit == 0 ? "%.0f %s" : "%.1f %s", v, kUnits[unit]);
    return buf;
}

std::string formatCount(uint64_t n) {
    char raw[32];
    std::snprintf(raw, sizeof(raw), "%" PRIu64, n);
    const std::string digits = raw;

    // Built back to front, which avoids having to work out where the first
    // separator goes from the length.
    std::string out;
    int since = 0;
    for (size_t i = digits.size(); i-- > 0;) {
        out += digits[i];
        if (++since % 3 == 0 && i > 0) out += ',';
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::string formatDuration(double seconds) {
    char buf[48];
    if (seconds < 1.0) {
        std::snprintf(buf, sizeof(buf), "%.0f ms", seconds * 1000.0);
    } else if (seconds < 60.0) {
        std::snprintf(buf, sizeof(buf), "%.1f s", seconds);
    } else {
        const int mins = static_cast<int>(seconds) / 60;
        std::snprintf(buf, sizeof(buf), "%dm %02ds", mins, static_cast<int>(seconds) % 60);
    }
    return buf;
}

std::string formatTime(int64_t unixTime) {
    if (unixTime <= 0) return "-";
    const std::time_t t = static_cast<std::time_t>(unixTime);
    std::tm tm {};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}
