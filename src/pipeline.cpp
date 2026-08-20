#include "pipeline.h"

#include <fnmatch.h>

#include "util.h"

namespace {

std::string basenameOf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Every pattern is tried against the whole path and against the filename, so
// both "*.zip" and "/mnt/scratch/*" do what they look like they do.
bool patternMatches(const CompiledPattern& p, const std::string& path, const std::string& name) {
    if (p.isRegex) {
        return std::regex_search(path, p.re) || std::regex_search(name, p.re);
    }
    return ::fnmatch(p.pattern.c_str(), path.c_str(), 0) == 0 ||
           ::fnmatch(p.pattern.c_str(), name.c_str(), 0) == 0;
}

// Any/All over the pattern set, before include or exclude is applied to it.
bool patternSetMatches(const std::vector<CompiledPattern>& patterns, PatternCombine combine,
                       const std::string& path) {
    const std::string name = basenameOf(path);
    if (combine == PatternCombine::All) {
        for (const auto& p : patterns) {
            if (!patternMatches(p, path, name)) return false;
        }
        return true;
    }
    for (const auto& p : patterns) {
        if (patternMatches(p, path, name)) return true;
    }
    return false;
}

}  // namespace

const char* const kRuleKindNames[kRuleKindCount] = {
    "glob pattern",  "regex pattern", "smallest size", "largest size",     "same filename",
    "same mtime",    "first bytes",   "full content hash", "exact byte compare"};

const char* const kPatternCombineNames[2] = {"OR", "AND"};
const char* const kPatternSelectNames[2] = {"EXCLUDE", "INCLUDE"};

const char* ruleKindName(RuleKind kind) {
    const int i = static_cast<int>(kind);
    return (i >= 0 && i < kRuleKindCount) ? kRuleKindNames[i] : "?";
}

bool ruleIsDrop(RuleKind kind) {
    switch (kind) {
        case RuleKind::Glob:
        case RuleKind::Regex:
        case RuleKind::MinSize:
        case RuleKind::MaxSize: return true;
        default: return false;
    }
}

const char* ruleKindHint(RuleKind kind) {
    switch (kind) {
        case RuleKind::Glob:
            return "shell glob, matched against the whole path and the filename";
        case RuleKind::Regex:
            return "ECMAScript regex, searched in the whole path and in the filename";
        case RuleKind::MinSize:
            return "files below this never enter the scan; 1 keeps out zero-byte files, which "
                   "are all identical to each other";
        case RuleKind::MaxSize: return "files above this never enter the scan; 0 means no ceiling";
        case RuleKind::SameName: return "free, and can only narrow a result, never widen one";
        case RuleKind::SameMtime: return "free, and can only narrow a result, never widen one";
        case RuleKind::HeadBytes:
            return "one seek per file, and where nearly every same-size coincidence dies\n"
                   "a file no bigger than this is hashed in full here and skips the next row";
        case RuleKind::FullHash:
            return "streams the whole file, and skips it entirely on a cache hit";
        case RuleKind::ExactBytes:
            return "the only row that proves a match rather than strongly suggesting one\n"
                   "without it, an irreversible delete rests on a 64-bit hash";
    }
    return "";
}

Pipeline defaultPipeline() {
    Pipeline p;
    p.rules.push_back(Rule {true, RuleKind::MinSize, {}, 1});
    p.rules.push_back(Rule {true, RuleKind::HeadBytes, {}, 65536});
    p.rules.push_back(Rule {true, RuleKind::FullHash, {}, 0});
    p.rules.push_back(Rule {true, RuleKind::ExactBytes, {}, 0});
    return p;
}

bool regexIsValid(const std::string& pattern, std::string& err) {
    if (pattern.empty()) {
        err = "empty pattern";
        return false;
    }
    try {
        std::regex re(pattern, std::regex::ECMAScript | std::regex::optimize);
        (void)re;
    } catch (const std::regex_error& e) {
        err = e.what();
        return false;
    }
    return true;
}

CompiledPipeline compilePipeline(const Pipeline& p) {
    CompiledPipeline c;
    c.combine = p.combine;
    c.select = p.select;
    c.threads = p.threads < 1 ? 1 : (p.threads > 16 ? 16 : p.threads);

    bool seen[kRuleKindCount] = {};

    for (const auto& r : p.rules) {
        if (!r.enabled) continue;

        if (r.kind == RuleKind::Glob || r.kind == RuleKind::Regex) {
            if (r.pattern.empty()) continue;
            CompiledPattern cp;
            cp.isRegex = (r.kind == RuleKind::Regex);
            cp.pattern = r.pattern;
            if (cp.isRegex) {
                try {
                    cp.re.assign(r.pattern, std::regex::ECMAScript | std::regex::optimize);
                } catch (const std::regex_error& e) {
                    // A rule that cannot be built is louder than a rule that is
                    // silently ignored, and the scan can still run without it.
                    c.problems.push_back("regex \"" + r.pattern + "\" was ignored: " + e.what());
                    continue;
                }
            }
            c.patterns.push_back(std::move(cp));
            continue;
        }

        const int slot = static_cast<int>(r.kind);
        if (seen[slot]) {
            c.problems.push_back(std::string("a second \"") + ruleKindName(r.kind) +
                                 "\" row was ignored; the first one already did the work");
            continue;
        }
        seen[slot] = true;

        switch (r.kind) {
            case RuleKind::MinSize: c.minSize = r.number; break;
            case RuleKind::MaxSize: c.maxSize = r.number; break;
            case RuleKind::HeadBytes: {
                Rule use = r;
                if (use.number < 512) use.number = 512;
                c.splits.push_back(use);
                break;
            }
            default: c.splits.push_back(r); break;
        }
    }

    if (c.maxSize > 0 && c.maxSize < c.minSize) {
        c.problems.push_back("the size ceiling is below the size floor, so nothing can match");
    }
    return c;
}

bool patternsAllow(const std::string& path, const CompiledPipeline& c) {
    if (c.patterns.empty()) return true;
    const bool matched = patternSetMatches(c.patterns, c.combine, path);
    return c.select == PatternSelect::Include ? matched : !matched;
}

bool sizeAllowed(uint64_t size, const CompiledPipeline& c) {
    if (size < c.minSize) return false;
    return c.maxSize == 0 || size <= c.maxSize;
}

bool fileAllowed(const std::string& path, uint64_t size, const CompiledPipeline& c) {
    return sizeAllowed(size, c) && patternsAllow(path, c);
}

bool dirPruned(const std::string& path, const CompiledPipeline& c) {
    if (c.patterns.empty() || c.select != PatternSelect::Exclude) return false;
    return patternSetMatches(c.patterns, c.combine, path);
}

std::string describePipeline(const CompiledPipeline& c) {
    std::string out = "size";

    if (c.minSize > 0) out += ", at least " + formatSize(c.minSize);
    if (c.maxSize > 0) out += ", at most " + formatSize(c.maxSize);

    if (!c.patterns.empty()) {
        out += ", ";
        for (size_t i = 0; i < c.patterns.size(); ++i) {
            if (i > 0) out += c.combine == PatternCombine::All ? " and " : " or ";
            out += c.patterns[i].pattern;
        }
        out += c.select == PatternSelect::Include ? " (include)" : " (exclude)";
    }

    for (const auto& r : c.splits) {
        out += ", ";
        if (r.kind == RuleKind::HeadBytes) {
            out += "first " + std::to_string(r.number) + " bytes";
        } else {
            out += ruleKindName(r.kind);
        }
    }
    return out;
}
