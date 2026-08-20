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

// What a v1 file recorded, held until the whole file is read so it can be
// turned into a pipeline in one go rather than a rule at a time.
struct LegacyV1 {
    bool seen = false;
    uint64_t minSize = 1;
    std::string excludeGlobs;
    bool sameName = false;
    bool sameMtime = false;
    bool headBytes = true;
    uint64_t headSize = 65536;
    bool fullHash = true;
    bool exactCompare = true;
    int threads = 4;
};

std::vector<std::string> splitGlobList(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\n' || c == ',') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else if (c != ' ' || !cur.empty()) {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// The v1 cascade, spelled out in the order it always ran. An exclude glob list
// becomes exclude-mode glob rows, which is exactly what it meant.
Pipeline pipelineFromLegacy(const LegacyV1& v1) {
    Pipeline p;
    p.combine = PatternCombine::Any;
    p.select = PatternSelect::Exclude;
    p.threads = v1.threads;

    p.rules.push_back(Rule {true, RuleKind::MinSize, {}, v1.minSize});
    for (const auto& g : splitGlobList(v1.excludeGlobs)) {
        p.rules.push_back(Rule {true, RuleKind::Glob, g, 0});
    }
    if (v1.sameName) p.rules.push_back(Rule {true, RuleKind::SameName, {}, 0});
    if (v1.sameMtime) p.rules.push_back(Rule {true, RuleKind::SameMtime, {}, 0});
    p.rules.push_back(Rule {v1.headBytes, RuleKind::HeadBytes, {}, v1.headSize});
    p.rules.push_back(Rule {v1.fullHash, RuleKind::FullHash, {}, 0});
    p.rules.push_back(Rule {v1.exactCompare, RuleKind::ExactBytes, {}, 0});
    return p;
}

}  // namespace

std::string sessionPath() { return stateDir() + "/session.tsv"; }

std::string serializeSession(const SessionData& d) {
    std::ostringstream os;
    os << "v2\n";

    for (const auto& r : d.roots) os << "root\t" << escapeField(r) << '\n';

    os << "scope\t" << b(d.scope.includeHidden) << '\t' << b(d.scope.collapseHardlinks) << '\n';

    os << "pipe\t" << static_cast<int>(d.pipeline.combine) << '\t'
       << static_cast<int>(d.pipeline.select) << '\t' << d.pipeline.threads << '\t'
       << static_cast<int>(d.pipeline.report) << '\n';
    // The count is written even when it is zero, because an empty rule list is a
    // real choice and no rule lines at all would be indistinguishable from a
    // file that predates them.
    os << "rules\t" << d.pipeline.rules.size() << '\n';
    // One line per rule, in list order, because list order is the pipeline.
    for (const auto& r : d.pipeline.rules) {
        os << "rule\t" << b(r.enabled) << '\t' << static_cast<int>(r.kind) << '\t' << r.number
           << '\t' << escapeField(r.pattern) << '\n';
    }

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
    if (!std::getline(is, line)) return d;

    const bool v1 = (line == "v1");
    if (!v1 && line != "v2") return d;

    LegacyV1 legacy;
    bool sawRule = false;

    while (std::getline(is, line)) {
        if (line.empty()) continue;
        const std::vector<std::string> f = splitTabs(line);
        const std::string& kind = f[0];

        if (kind == "root" && f.size() >= 2) {
            d.roots.push_back(unescapeField(f[1]));
        } else if (kind == "scope" && v1 && f.size() >= 5) {
            legacy.seen = true;
            legacy.minSize = std::strtoull(f[1].c_str(), nullptr, 10);
            d.scope.includeHidden = toBool(f[2]);
            d.scope.collapseHardlinks = toBool(f[3]);
            legacy.excludeGlobs = unescapeField(f[4]);
        } else if (kind == "scope" && !v1 && f.size() >= 3) {
            d.scope.includeHidden = toBool(f[1]);
            d.scope.collapseHardlinks = toBool(f[2]);
        } else if (kind == "stage" && v1 && f.size() >= 8) {
            legacy.seen = true;
            legacy.sameName = toBool(f[1]);
            legacy.sameMtime = toBool(f[2]);
            legacy.headBytes = toBool(f[3]);
            legacy.headSize = std::strtoull(f[4].c_str(), nullptr, 10);
            if (legacy.headSize < 512) legacy.headSize = 512;
            legacy.fullHash = toBool(f[5]);
            legacy.exactCompare = toBool(f[6]);
            legacy.threads = clampInt(f[7], 1, 16, 4);
        } else if (kind == "pipe" && f.size() >= 4) {
            d.pipeline.combine = static_cast<PatternCombine>(clampInt(f[1], 0, 1, 0));
            d.pipeline.select = static_cast<PatternSelect>(clampInt(f[2], 0, 1, 0));
            d.pipeline.threads = clampInt(f[3], 1, 16, 4);
            // Appended after the first v2 files were written, so its absence
            // means the default rather than a broken record.
            if (f.size() >= 5) {
                d.pipeline.report = static_cast<ReportMode>(clampInt(f[4], 0, 1, 0));
            }
        } else if (kind == "rules") {
            // The list that follows replaces the default one rather than adding
            // to it, so a saved pipeline is what comes back, not a merge.
            d.pipeline.rules.clear();
            sawRule = true;
        } else if (kind == "rule" && f.size() >= 5) {
            if (!sawRule) {
                d.pipeline.rules.clear();
                sawRule = true;
            }
            Rule r;
            r.enabled = toBool(f[1]);
            r.kind = static_cast<RuleKind>(clampInt(f[2], 0, kRuleKindCount - 1, 0));
            r.number = std::strtoull(f[3].c_str(), nullptr, 10);
            r.pattern = unescapeField(f[4]);
            d.pipeline.rules.push_back(std::move(r));
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

    if (v1 && legacy.seen) d.pipeline = pipelineFromLegacy(legacy);
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
