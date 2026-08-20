#include "session.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

#include "util.h"

namespace fs = std::filesystem;

namespace {

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == '\t') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

const char* b(bool v) { return v ? "1" : "0"; }
bool toBool(const std::string& s) { return s == "1"; }

int clampInt(const std::string& s, int lo, int hi, int fallback) {
    const int v = std::atoi(s.c_str());
    return (v >= lo && v <= hi) ? v : fallback;
}

}  // namespace

std::string sessionPath() { return stateDir() + "/session.tsv"; }

std::string serializeSession(const SessionData& d) {
    std::ostringstream os;
    os << "v1\n";

    for (const auto& r : d.roots) os << "root\t" << escapeField(r) << '\n';

    os << "scope\t" << d.scope.minSize << '\t' << b(d.scope.includeHidden) << '\t'
       << b(d.scope.collapseHardlinks) << '\t' << escapeField(d.scope.excludeGlobs) << '\n';

    os << "stage\t" << b(d.stages.sameName) << '\t' << b(d.stages.sameMtime) << '\t'
       << b(d.stages.headBytes) << '\t' << d.stages.headSize << '\t' << b(d.stages.fullHash)
       << '\t' << b(d.stages.exactCompare) << '\t' << d.stages.hashThreads << '\n';

    os << "tie\t" << static_cast<int>(d.tie) << '\n';
    os << "quar\t" << escapeField(d.quarantineRoot) << '\n';
    os << "act\t" << static_cast<int>(d.action) << '\n';
    os << "view\t" << static_cast<int>(d.sort) << '\t' << d.filter.minSize << '\t'
       << d.filter.minMembers << '\t' << b(d.showLog) << '\t' << escapeField(d.filter.text)
       << '\n';
    return os.str();
}

SessionData parseSession(const std::string& text) {
    SessionData d;
    std::istringstream is(text);
    std::string line;
    if (!std::getline(is, line) || line != "v1") return d;

    while (std::getline(is, line)) {
        if (line.empty()) continue;
        const std::vector<std::string> f = splitTabs(line);
        const std::string& kind = f[0];

        if (kind == "root" && f.size() >= 2) {
            d.roots.push_back(unescapeField(f[1]));
        } else if (kind == "scope" && f.size() >= 5) {
            d.scope.minSize = std::strtoull(f[1].c_str(), nullptr, 10);
            d.scope.includeHidden = toBool(f[2]);
            d.scope.collapseHardlinks = toBool(f[3]);
            d.scope.excludeGlobs = unescapeField(f[4]);
        } else if (kind == "stage" && f.size() >= 8) {
            d.stages.sameName = toBool(f[1]);
            d.stages.sameMtime = toBool(f[2]);
            d.stages.headBytes = toBool(f[3]);
            d.stages.headSize = std::strtoull(f[4].c_str(), nullptr, 10);
            if (d.stages.headSize < 512) d.stages.headSize = 512;
            d.stages.fullHash = toBool(f[5]);
            d.stages.exactCompare = toBool(f[6]);
            d.stages.hashThreads = clampInt(f[7], 1, 16, 4);
        } else if (kind == "tie" && f.size() >= 2) {
            d.tie = static_cast<TieBreak>(clampInt(f[1], 0, 4, 0));
        } else if (kind == "quar" && f.size() >= 2) {
            d.quarantineRoot = unescapeField(f[1]);
        } else if (kind == "act" && f.size() >= 2) {
            d.action = static_cast<ActionKind>(clampInt(f[1], 0, 1, 1));
        } else if (kind == "view" && f.size() >= 6) {
            d.sort = static_cast<GroupSort>(clampInt(f[1], 0, 3, 0));
            d.filter.minSize = std::strtoull(f[2].c_str(), nullptr, 10);
            d.filter.minMembers = clampInt(f[3], 2, 999, 2);
            d.showLog = toBool(f[4]);
            d.filter.text = unescapeField(f[5]);
        }
    }
    return d;
}

bool saveSessionIfChanged(const std::string& path, const std::string& text, size_t& lastHash) {
    const size_t hash = std::hash<std::string> {}(text);
    if (hash == lastHash) return false;

    if (!ensureDir(fs::path(path).parent_path().string())) return false;

    // Write to a sibling then rename, so a crash mid-write cannot leave a
    // truncated session file behind.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        out << text;
        if (!out) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) return false;

    lastHash = hash;
    return true;
}

std::string readFileOrEmpty(const std::string& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream os;
    os << in.rdbuf();
    return os.str();
}
