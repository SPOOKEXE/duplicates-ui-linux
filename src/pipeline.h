#pragma once

#include <cstdint>
#include <regex>
#include <string>
#include <vector>

// One user-ordered list decides the whole scan. Two kinds of row live in it:
//
//   drop rows  look at one file and say yes or no  (glob, regex, size bounds)
//   split rows need two files to mean anything, and cut a candidate set into
//              smaller ones                        (name, mtime, bytes, hashes)
//
// They share a list because they commute: throwing a file away never changes
// whether two other files are copies of each other. That is also why the drop
// rows are applied during the walk however the user orders them, which costs
// nothing and lets an excluded directory be skipped whole.
enum class RuleKind {
    Glob,        // drop
    Regex,       // drop
    MinSize,     // drop
    MaxSize,     // drop
    SameName,    // split
    SameMtime,   // split
    HeadBytes,   // split
    FullHash,    // split
    ExactBytes,  // split
};

constexpr int kRuleKindCount = 9;
extern const char* const kRuleKindNames[kRuleKindCount];

const char* ruleKindName(RuleKind kind);
bool ruleIsDrop(RuleKind kind);
// A one-line reminder of what the row costs and what it proves, for the tooltip.
const char* ruleKindHint(RuleKind kind);

// How the glob and regex rows combine with each other, and what a match means.
// These are properties of the pattern set as a whole rather than of one row,
// because "any of these" and "all of these" is not a per-row question.
enum class PatternCombine { Any, All };     // OR, AND
enum class PatternSelect { Exclude, Include };

extern const char* const kPatternCombineNames[2];
extern const char* const kPatternSelectNames[2];

struct Rule {
    bool enabled = true;
    RuleKind kind = RuleKind::Glob;
    std::string pattern;   // Glob and Regex only
    uint64_t number = 0;   // MinSize/MaxSize bytes, HeadBytes window
};

struct Pipeline {
    std::vector<Rule> rules;
    PatternCombine combine = PatternCombine::Any;
    PatternSelect select = PatternSelect::Exclude;
    int threads = 4;
};

// The list a fresh session starts with: the old hardcoded cascade, spelled out.
Pipeline defaultPipeline();

// A pattern row ready to be matched, with its regex already built. Compiling a
// regex per file would cost more than reading the file.
struct CompiledPattern {
    bool isRegex = false;
    std::string pattern;
    std::regex re;
};

// The pipeline flattened into the form the walk and the cascade actually want.
// Built once per scan.
struct CompiledPipeline {
    std::vector<CompiledPattern> patterns;
    PatternCombine combine = PatternCombine::Any;
    PatternSelect select = PatternSelect::Exclude;

    uint64_t minSize = 0;
    uint64_t maxSize = 0;  // 0 means no ceiling
    std::vector<Rule> splits;  // ordered, deduplicated, enabled only
    int threads = 4;

    // Rows that were dropped or could not be built, so the log can say why
    // rather than the user wondering where their rule went.
    std::vector<std::string> problems;
};

// Disabled rows are dropped. Pattern rows accumulate; every other kind keeps
// only its first enabled row, because a second "full content hash" or a second
// size floor has nothing left to do.
CompiledPipeline compilePipeline(const Pipeline& p);

// True when path is a regex the standard library will accept. Used by the UI to
// mark a row red while it is being typed, instead of throwing mid-scan.
bool regexIsValid(const std::string& pattern, std::string& err);

// True when the path survives the glob and regex rows. An empty pattern set
// keeps everything, in include mode as much as in exclude mode: "include
// nothing" is never what an empty list means.
bool patternsAllow(const std::string& path, const CompiledPipeline& c);

// True when the size is inside the floor and ceiling rows.
bool sizeAllowed(uint64_t size, const CompiledPipeline& c);

// Both of the above. The walk applies them separately so it can count which
// rule turned a file away, but anything else wants the single answer.
bool fileAllowed(const std::string& path, uint64_t size, const CompiledPipeline& c);

// Whether a directory can be skipped without opening it. Only exclude patterns
// can answer this: an include set says nothing about which folders might hold a
// match, so in include mode every directory is still walked.
bool dirPruned(const std::string& path, const CompiledPipeline& c);

// "size, *.zip and *.rar (include, any), first 65536 bytes, exact byte compare"
std::string describePipeline(const CompiledPipeline& c);
