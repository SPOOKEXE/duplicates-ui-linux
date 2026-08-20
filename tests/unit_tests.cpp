// One binary, one assert macro, no framework: the same spirit as the rest of
// the project. Every test builds its own fixture tree in a temporary directory
// and removes it afterwards, so the suite is safe to run anywhere.
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "actions.h"
#include "cascade.h"
#include "dupes.h"
#include "groups.h"
#include "hash.h"
#include "hash_cache.h"
#include "log.h"
#include "pipeline.h"
#include "runs.h"
#include "scanner.h"
#include "session.h"
#include "util.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;
int checks = 0;
const char* currentTest = "";

void check(bool ok, const char* expr, int line) {
    ++checks;
    if (ok) return;
    ++failures;
    std::printf("  FAIL %s:%d  %s\n", currentTest, line, expr);
}

#define CHECK(cond) check((cond), #cond, __LINE__)

void checkEq(uint64_t got, uint64_t want, const char* expr, int line) {
    ++checks;
    if (got == want) return;
    ++failures;
    std::printf("  FAIL %s:%d  %s: got %llu, want %llu\n", currentTest, line, expr,
                static_cast<unsigned long long>(got), static_cast<unsigned long long>(want));
}

#define CHECK_EQ(got, want) checkEq((got), (want), #got, __LINE__)

struct TempDir {
    std::string path;

    TempDir() {
        char t[] = "/tmp/dupui-test-XXXXXX";
        path = ::mkdtemp(t);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void writeFile(const std::string& path, const std::string& content) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

std::string repeat(char c, size_t n) { return std::string(n, c); }

bool exists(const std::string& p) {
    std::error_code ec;
    return fs::exists(fs::symlink_status(p, ec));
}

// ---------------------------------------------------------------- hashing

void testHash() {
    currentTest = "hash";
    // The published XXH64 vectors, which pin down the primes, the tail handling
    // and the avalanche all at once.
    CHECK(xxhash64("", 0) == 0xEF46DB3751D8E999ULL);
    CHECK(xxhash64("a", 1) == 0xD24EC4F1A98C6E5BULL);
    CHECK(xxhash64("abc", 3) == 0x44BC2CF5AD770999ULL);

    // The streaming form has to agree with the one-shot form at every possible
    // split, because a file is fed to it in arbitrary chunks.
    std::string big;
    for (int i = 0; i < 500; ++i) big += static_cast<char>('a' + i % 26);
    bool agrees = true;
    for (size_t cut = 0; cut <= big.size(); ++cut) {
        XxHash64 h;
        h.update(big.data(), cut);
        h.update(big.data() + cut, big.size() - cut);
        if (h.digest() != xxhash64(big.data(), big.size())) agrees = false;
    }
    CHECK(agrees);

    TempDir dir;
    writeFile(dir.path + "/f", "0123456789");
    uint64_t head = 0, full = 0;
    CHECK(hashHead(dir.path + "/f", 4, head));
    CHECK(hashFull(dir.path + "/f", nullptr, full));
    CHECK(head == xxhash64("0123", 4));
    CHECK(full == xxhash64("0123456789", 10));
    CHECK(!hashFull(dir.path + "/missing", nullptr, full));

    writeFile(dir.path + "/same", "0123456789");
    writeFile(dir.path + "/diff", "0123456789x");
    bool err = false;
    CHECK(sameContents(dir.path + "/f", dir.path + "/same", nullptr, err) && !err);
    CHECK(!sameContents(dir.path + "/f", dir.path + "/diff", nullptr, err));
}

// ---------------------------------------------------------------- cascade

// Two roots holding: one real duplicate pair, one pair that shares its first bytes
// but not its tail, one pair that shares only its size, a hardlinked pair, a
// zero-byte pair and a symlink.
void buildFixture(const std::string& base) {
    const std::string a = base + "/a", b = base + "/b";

    writeFile(a + "/same.bin", repeat('m', 1000));
    writeFile(b + "/same.bin", repeat('m', 1000));

    writeFile(a + "/small.txt", "abc");
    writeFile(b + "/small_copy.txt", "abc");

    writeFile(a + "/tail.bin", repeat('h', 64) + repeat('A', 136));
    writeFile(b + "/tail.bin", repeat('h', 64) + repeat('B', 136));

    writeFile(a + "/uniq.bin", repeat('q', 1000));
    writeFile(b + "/sizeonly.bin", repeat('z', 1000));

    writeFile(a + "/empty.bin", "");
    writeFile(b + "/empty.bin", "");

    fs::create_hard_link(b + "/same.bin", b + "/hardlink.bin");
    fs::create_symlink(a + "/same.bin", b + "/symlink.bin");
}

struct ScanOutcome {
    std::vector<FileEntry> files;
    std::vector<DupGroup> groups;
    WalkStats walk;
};

// The knobs the fixed cascade used to expose, so a test can tweak one row
// without spelling out the whole rule list.
struct StageOptions {
    bool sameName = false;
    bool sameMtime = false;
    bool headBytes = true;
    uint64_t headSize = 65536;
    bool fullHash = true;
    bool exactCompare = true;
    uint64_t minSize = 1;
    std::string patterns;  // comma separated
    bool regex = false;
    PatternCombine combine = PatternCombine::Any;
    PatternSelect select = PatternSelect::Exclude;
};

Pipeline pipelineFor(const StageOptions& o) {
    Pipeline p;
    p.threads = 2;
    p.combine = o.combine;
    p.select = o.select;
    p.rules.push_back(Rule {true, RuleKind::MinSize, {}, o.minSize});

    std::string cur;
    const auto flush = [&] {
        if (cur.empty()) return;
        p.rules.push_back(Rule {true, o.regex ? RuleKind::Regex : RuleKind::Glob, cur, 0});
        cur.clear();
    };
    for (char c : o.patterns) {
        if (c == ',') {
            flush();
        } else {
            cur += c;
        }
    }
    flush();

    if (o.sameName) p.rules.push_back(Rule {true, RuleKind::SameName, {}, 0});
    if (o.sameMtime) p.rules.push_back(Rule {true, RuleKind::SameMtime, {}, 0});
    if (o.headBytes) p.rules.push_back(Rule {true, RuleKind::HeadBytes, {}, o.headSize});
    if (o.fullHash) p.rules.push_back(Rule {true, RuleKind::FullHash, {}, 0});
    if (o.exactCompare) p.rules.push_back(Rule {true, RuleKind::ExactBytes, {}, 0});
    return p;
}

ScanOutcome scanPipeline(const std::string& base, const Pipeline& pipeline, ScopeFilters scope,
                         TieBreak tie = TieBreak::OldestMtime) {
    ScanOutcome out;
    const CompiledPipeline compiled = compilePipeline(pipeline);
    WalkHooks hooks;
    out.files = walkRoots({base + "/a", base + "/b"}, scope, compiled, hooks, out.walk);
    if (scope.collapseHardlinks) collapseHardlinks(out.files, tie, out.walk);

    CascadeHooks ch;
    out.groups = runCascade(out.files, compiled, ch);
    resolveKeepers(out.groups, out.files, tie);
    return out;
}

ScanOutcome scanFixture(const std::string& base, StageOptions stages, ScopeFilters scope,
                        TieBreak tie = TieBreak::OldestMtime) {
    return scanPipeline(base, pipelineFor(stages), scope, tie);
}

// Finds the group whose keeper path ends with `suffix`, or -1.
int findGroup(const ScanOutcome& o, const std::string& suffix) {
    for (size_t i = 0; i < o.groups.size(); ++i) {
        for (const auto& m : o.groups[i].members) {
            const std::string& p = o.files[m.fileIndex].path;
            if (p.size() >= suffix.size() &&
                p.compare(p.size() - suffix.size(), suffix.size(), suffix) == 0) {
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}

void testCascade() {
    currentTest = "cascade";
    TempDir dir;
    buildFixture(dir.path);

    StageOptions stages;
    stages.headSize = 64;  // small enough that the fixture can exercise both sides of it
    ScopeFilters scope;

    const ScanOutcome o = scanFixture(dir.path, stages, scope);

    // Zero-byte files are below the size floor, and the symlink is never
    // followed, so neither can reach a group.
    CHECK_EQ(o.walk.skippedSmall, 2);
    CHECK_EQ(o.walk.skippedSymlink, 1);
    CHECK_EQ(o.walk.collapsedLinks, 1);

    // Exactly two real duplicate pairs exist in the fixture.
    CHECK_EQ(o.groups.size(), 2);

    const int sameGroup = findGroup(o, "/a/same.bin");
    CHECK(sameGroup >= 0);
    if (sameGroup >= 0) {
        CHECK_EQ(o.groups[sameGroup].members.size(), 2);
        CHECK_EQ(o.groups[sameGroup].size, 1000);
        // Priority: the copy in the first input directory is the one kept.
        const FileEntry& keeper = o.files[o.groups[sameGroup].members[o.groups[sameGroup].keeper].fileIndex];
        CHECK_EQ(static_cast<uint64_t>(keeper.rootIndex), 0);
        CHECK(!o.groups[sameGroup].members[o.groups[sameGroup].keeper].selected);
    }

    const int smallGroup = findGroup(o, "/a/small.txt");
    CHECK(smallGroup >= 0);
    if (smallGroup >= 0) CHECK_EQ(o.groups[smallGroup].members.size(), 2);

    // Same size and same first 64 bytes, different tail: must not group.
    CHECK(findGroup(o, "/a/tail.bin") < 0);
    // Same size, different content from the first byte: must not group.
    CHECK(findGroup(o, "/a/uniq.bin") < 0);

    // The file that survived the collapse carries the other name.
    bool foundLink = false;
    for (const auto& f : o.files) {
        if (!f.alsoLinkedAt.empty()) foundLink = true;
    }
    CHECK(foundLink);
}

void testHeadIsFullShortcut() {
    currentTest = "head-is-full";
    TempDir dir;
    writeFile(dir.path + "/a/x", "abc");
    writeFile(dir.path + "/b/x", "abc");

    StageOptions stages;
    stages.headSize = 64;  // both files are far smaller
    ScopeFilters scope;
    const ScanOutcome o = scanFixture(dir.path, stages, scope);

    CHECK_EQ(o.groups.size(), 1);
    // A file no larger than the head window is hashed in full by the head stage,
    // which is what lets the expensive stage skip it entirely.
    for (const auto& f : o.files) {
        CHECK(f.headIsFull);
        CHECK(f.hashed);
        CHECK(f.fullHash == f.headHash);
    }
}

void testExactCompareSplitsBucket() {
    currentTest = "exact-compare";
    TempDir dir;
    // Same size, and the head stage is switched off, so the only thing that can
    // separate these two is the byte comparison itself.
    writeFile(dir.path + "/a/x", repeat('p', 500));
    writeFile(dir.path + "/b/x", repeat('r', 500));

    StageOptions stages;
    stages.headBytes = false;
    stages.fullHash = false;
    stages.exactCompare = true;
    ScopeFilters scope;

    const ScanOutcome o = scanFixture(dir.path, stages, scope);
    CHECK_EQ(o.groups.size(), 0);

    // Without it, size alone would have called them duplicates, which is exactly
    // why the warning exists in the interface.
    StageOptions unsafe = stages;
    unsafe.exactCompare = false;
    const ScanOutcome bad = scanFixture(dir.path, unsafe, scope);
    CHECK_EQ(bad.groups.size(), 1);
}

void testHardlinkCollapseOff() {
    currentTest = "hardlink-off";
    TempDir dir;
    writeFile(dir.path + "/a/x", repeat('m', 200));
    fs::create_directories(dir.path + "/b");
    fs::create_hard_link(dir.path + "/a/x", dir.path + "/b/x");

    StageOptions stages;
    ScopeFilters scope;
    scope.collapseHardlinks = false;

    const ScanOutcome on = scanFixture(dir.path, stages, scope);
    // With the fold off, two names for one inode are ordinary, actionable copies.
    CHECK_EQ(on.groups.size(), 1);

    scope.collapseHardlinks = true;
    const ScanOutcome off = scanFixture(dir.path, stages, scope);
    // With it on, they occupy the disk once and there is nothing to reclaim.
    CHECK_EQ(off.groups.size(), 0);
    CHECK_EQ(off.walk.collapsedLinks, 1);
}

void testExcludeGlobsAndHidden() {
    currentTest = "scope-filters";
    TempDir dir;
    writeFile(dir.path + "/a/keep.bin", repeat('k', 300));
    writeFile(dir.path + "/b/keep.bin", repeat('k', 300));
    writeFile(dir.path + "/a/.hidden.bin", repeat('k', 300));
    writeFile(dir.path + "/a/skip.tmp", repeat('t', 300));
    writeFile(dir.path + "/b/skip.tmp", repeat('t', 300));

    StageOptions stages;
    stages.patterns = "*.tmp";
    ScopeFilters scope;
    const ScanOutcome o = scanFixture(dir.path, stages, scope);
    CHECK_EQ(o.groups.size(), 1);
    CHECK_EQ(o.walk.skippedFiltered, 2);
    CHECK_EQ(o.walk.skippedHidden, 1);

    stages.patterns.clear();
    scope.includeHidden = true;
    const ScanOutcome withHidden = scanFixture(dir.path, stages, scope);
    // keep.bin x2 plus the hidden copy is one group of three, and the two .tmp
    // files are now a group of their own.
    CHECK_EQ(withHidden.groups.size(), 2);
    const int g = findGroup(withHidden, "/a/keep.bin");
    CHECK(g >= 0);
    if (g >= 0) CHECK_EQ(withHidden.groups[g].members.size(), 3);
}

void testIncludeAndCombineModes() {
    currentTest = "filter-modes";
    TempDir dir;
    writeFile(dir.path + "/a/keep.zip", repeat('z', 300));
    writeFile(dir.path + "/b/keep.zip", repeat('z', 300));
    writeFile(dir.path + "/a/other.txt", repeat('t', 300));
    writeFile(dir.path + "/b/other.txt", repeat('t', 300));

    ScopeFilters scope;

    // Include mode keeps only what matches, which is the whole point of asking
    // for one archive format and nothing else.
    StageOptions inc;
    inc.patterns = "*.zip";
    inc.select = PatternSelect::Include;
    const ScanOutcome only = scanFixture(dir.path, inc, scope);
    CHECK_EQ(only.groups.size(), 1);
    CHECK_EQ(only.walk.skippedFiltered, 2);
    CHECK(findGroup(only, "/a/keep.zip") >= 0);

    // Several include patterns are an OR by default: any of them is enough.
    StageOptions two = inc;
    two.patterns = "*.zip,*.txt";
    CHECK_EQ(scanFixture(dir.path, two, scope).groups.size(), 2);

    // The same two under AND can never both hold, so nothing survives.
    StageOptions both = two;
    both.combine = PatternCombine::All;
    const ScanOutcome none = scanFixture(dir.path, both, scope);
    CHECK_EQ(none.groups.size(), 0);
    CHECK_EQ(none.files.size(), 0);

    // An empty pattern list means "no filter", in include mode as much as in
    // exclude mode. Reading it as "include nothing" would hide everything.
    StageOptions empty;
    empty.select = PatternSelect::Include;
    CHECK_EQ(scanFixture(dir.path, empty, scope).groups.size(), 2);
}

void testRegexRules() {
    currentTest = "regex-rules";
    TempDir dir;
    writeFile(dir.path + "/a/movie.mkv", repeat('m', 400));
    writeFile(dir.path + "/b/movie.mkv", repeat('m', 400));
    writeFile(dir.path + "/a/notes.txt", repeat('n', 400));
    writeFile(dir.path + "/b/notes.txt", repeat('n', 400));

    ScopeFilters scope;
    StageOptions o;
    o.regex = true;
    o.patterns = "\\.(mkv|mp4)$";
    o.select = PatternSelect::Include;

    const ScanOutcome inc = scanFixture(dir.path, o, scope);
    CHECK_EQ(inc.groups.size(), 1);
    CHECK(findGroup(inc, "/a/movie.mkv") >= 0);

    o.select = PatternSelect::Exclude;
    const ScanOutcome exc = scanFixture(dir.path, o, scope);
    CHECK_EQ(exc.groups.size(), 1);
    CHECK(findGroup(exc, "/a/notes.txt") >= 0);

    // A regex that will not build is reported and skipped, and the rest of the
    // scan still runs. Losing a rule silently would be the worse failure.
    Pipeline broken;
    broken.rules.push_back(Rule {true, RuleKind::Regex, "([unclosed", 0});
    broken.rules.push_back(Rule {true, RuleKind::ExactBytes, {}, 0});
    const CompiledPipeline c = compilePipeline(broken);
    CHECK(c.patterns.empty());
    CHECK_EQ(c.problems.size(), 1);
    CHECK_EQ(c.splits.size(), 1);

    std::string err;
    CHECK(!regexIsValid("([unclosed", err));
    CHECK(regexIsValid("\\.zip$", err));
}

void testDirectoryPruning() {
    currentTest = "dir-pruning";
    TempDir dir;
    writeFile(dir.path + "/a/keep.bin", repeat('k', 300));
    writeFile(dir.path + "/b/keep.bin", repeat('k', 300));
    writeFile(dir.path + "/a/cache/junk.bin", repeat('j', 300));
    writeFile(dir.path + "/b/cache/junk.bin", repeat('j', 300));

    ScopeFilters scope;
    StageOptions excl;
    excl.patterns = "*/cache";

    const ScanOutcome pruned = scanFixture(dir.path, excl, scope);
    CHECK_EQ(pruned.groups.size(), 1);
    // The folders were skipped whole rather than walked and filtered per file.
    CHECK_EQ(pruned.walk.prunedDirs, 2);
    CHECK_EQ(pruned.walk.skippedFiltered, 0);

    // Include mode cannot prune: a folder name says nothing about whether the
    // files inside it will match, so every directory is still walked.
    StageOptions inc;
    inc.patterns = "*.bin";
    inc.select = PatternSelect::Include;
    const ScanOutcome walked = scanFixture(dir.path, inc, scope);
    CHECK_EQ(walked.walk.prunedDirs, 0);
    CHECK_EQ(walked.groups.size(), 2);
}

void testRuleOrderAndCompile() {
    currentTest = "rule-order";
    TempDir dir;
    writeFile(dir.path + "/a/x.bin", repeat('q', 900));
    writeFile(dir.path + "/b/x.bin", repeat('q', 900));
    writeFile(dir.path + "/a/y.bin", repeat('w', 900));
    writeFile(dir.path + "/b/z.bin", repeat('w', 900));

    ScopeFilters scope;

    // Same filename after the content rows rather than before them: it can only
    // ever narrow a set, so the answer must not depend on where it sits.
    Pipeline late;
    late.threads = 2;
    late.rules = {
        Rule {true, RuleKind::MinSize, {}, 1},
        Rule {true, RuleKind::HeadBytes, {}, 512},
        Rule {true, RuleKind::ExactBytes, {}, 0},
        Rule {true, RuleKind::SameName, {}, 0},
    };
    Pipeline early = late;
    early.rules = {late.rules[0], late.rules[3], late.rules[1], late.rules[2]};

    const ScanOutcome a = scanPipeline(dir.path, late, scope);
    const ScanOutcome b = scanPipeline(dir.path, early, scope);
    CHECK_EQ(a.groups.size(), 1);  // y.bin and z.bin match by content but not by name
    CHECK_EQ(b.groups.size(), 1);

    // A second row of a kind has nothing left to do, so it is dropped and said
    // out loud rather than run twice.
    Pipeline twice;
    twice.rules = {
        Rule {true, RuleKind::FullHash, {}, 0},
        Rule {true, RuleKind::FullHash, {}, 0},
    };
    const CompiledPipeline c = compilePipeline(twice);
    CHECK_EQ(c.splits.size(), 1);
    CHECK_EQ(c.problems.size(), 1);

    // Disabled rows never reach the compiled form at all.
    Pipeline off;
    off.rules = {Rule {false, RuleKind::Glob, "*.zip", 0}, Rule {false, RuleKind::ExactBytes, {}, 0}};
    const CompiledPipeline oc = compilePipeline(off);
    CHECK(oc.patterns.empty());
    CHECK(oc.splits.empty());

    // The head window has a floor: a smaller one would cost a seek per file and
    // buy almost nothing.
    Pipeline tiny;
    tiny.rules = {Rule {true, RuleKind::HeadBytes, {}, 16}};
    CHECK_EQ(compilePipeline(tiny).splits[0].number, 512);
}

void testSizeBounds() {
    currentTest = "size-bounds";
    TempDir dir;
    writeFile(dir.path + "/a/small.bin", repeat('s', 100));
    writeFile(dir.path + "/b/small.bin", repeat('s', 100));
    writeFile(dir.path + "/a/big.bin", repeat('g', 5000));
    writeFile(dir.path + "/b/big.bin", repeat('g', 5000));

    ScopeFilters scope;

    StageOptions floorOnly;
    floorOnly.minSize = 1000;
    const ScanOutcome big = scanFixture(dir.path, floorOnly, scope);
    CHECK_EQ(big.groups.size(), 1);
    CHECK_EQ(big.walk.skippedSmall, 2);
    CHECK(findGroup(big, "/a/big.bin") >= 0);

    // A ceiling is its own row, so it needs the pipeline spelled out.
    Pipeline capped;
    capped.threads = 2;
    capped.rules = {
        Rule {true, RuleKind::MinSize, {}, 1},
        Rule {true, RuleKind::MaxSize, {}, 1000},
        Rule {true, RuleKind::ExactBytes, {}, 0},
    };
    const ScanOutcome small = scanPipeline(dir.path, capped, scope);
    CHECK_EQ(small.groups.size(), 1);
    CHECK_EQ(small.walk.skippedLarge, 2);
    CHECK(findGroup(small, "/a/small.bin") >= 0);
}

void testLogBuffer() {
    currentTest = "log";
    Log log;
    log.info("walked");
    log.warn("careful");
    log.error("broken");

    const std::vector<LogLine> lines = log.lines();
    CHECK_EQ(lines.size(), 3);
    CHECK_EQ(log.count(LogLevel::Info), 1);
    CHECK_EQ(log.count(LogLevel::Error), 1);
    CHECK(lines[0].text == "walked");
    CHECK(lines[2].level == LogLevel::Error);
    CHECK(!lines[0].stamp.empty());
    CHECK_EQ(log.dropped(), 0);

    log.clear();
    CHECK(log.lines().empty());
    CHECK_EQ(log.count(LogLevel::Warn), 0);
}

void testChunkProgressCallbacks() {
    currentTest = "chunk-progress";
    TempDir dir;
    // Bigger than the 1 MB read buffer, so a single file spans several reads.
    // This is the whole point: without a per-chunk callback, a caller hears
    // nothing at all until a multi-gigabyte file reaches EOF.
    writeFile(dir.path + "/a.bin", repeat('c', 3 * 1024 * 1024 + 77));
    writeFile(dir.path + "/b.bin", repeat('c', 3 * 1024 * 1024 + 77));

    uint64_t calls = 0, reported = 0;
    const ChunkFn onChunk = [&](uint64_t n) {
        ++calls;
        reported += n;
    };

    uint64_t h = 0;
    std::atomic<uint64_t> bytes {0};
    CHECK(hashFull(dir.path + "/a.bin", nullptr, h, &bytes, &onChunk));
    CHECK(calls >= 3);
    CHECK_EQ(reported, 3u * 1024 * 1024 + 77);
    CHECK_EQ(bytes.load(), reported);

    calls = 0;
    reported = 0;
    bytes.store(0);
    bool err = false;
    CHECK(sameContents(dir.path + "/a.bin", dir.path + "/b.bin", nullptr, err, &bytes, &onChunk));
    CHECK(!err);
    CHECK(calls >= 3);
    // A comparison reads both sides, so it reports twice the file's size.
    CHECK_EQ(reported, 2u * (3u * 1024 * 1024 + 77));

    // The callback is optional, and omitting it must not change the answer.
    uint64_t plain = 0;
    CHECK(hashFull(dir.path + "/a.bin", nullptr, plain));
    CHECK_EQ(plain, h);
}

void testCascadeReportsBytesAndCurrentFile() {
    currentTest = "byte-progress";
    TempDir dir;
    writeFile(dir.path + "/a/big.bin", repeat('v', 400000));
    writeFile(dir.path + "/b/big.bin", repeat('v', 400000));

    Pipeline p;
    p.threads = 1;
    p.rules = {
        Rule {true, RuleKind::MinSize, {}, 1},
        Rule {true, RuleKind::HeadBytes, {}, 512},
        Rule {true, RuleKind::FullHash, {}, 0},
        Rule {true, RuleKind::ExactBytes, {}, 0},
    };
    const CompiledPipeline compiled = compilePipeline(p);

    WalkStats walk;
    WalkHooks wh;
    std::vector<FileEntry> files =
        walkRoots({dir.path + "/a", dir.path + "/b"}, ScopeFilters {}, compiled, wh, walk);

    // Every row that reads bytes has to declare how many, or the progress bar
    // falls back to counting files and freezes on a large one.
    uint64_t sawFullHashBytes = 0, sawExactBytes = 0;
    bool namedAFile = false;
    CascadeHooks ch;
    // The peak, not the last: a row emits once more when it finishes, with the
    // totals cleared because there is nothing left to read.
    ch.onProgress = [&](const CascadeProgress& prog) {
        if (prog.stage == Stage::FullHash) {
            sawFullHashBytes = std::max(sawFullHashBytes, prog.stageBytesTotal);
        }
        if (prog.stage == Stage::ExactCompare) {
            sawExactBytes = std::max(sawExactBytes, prog.stageBytesTotal);
        }
        if (!prog.current.empty()) namedAFile = true;
    };

    const std::vector<DupGroup> groups = runCascade(files, compiled, ch);
    CHECK_EQ(groups.size(), 1);
    CHECK_EQ(sawFullHashBytes, 800000);          // both files, read in full
    CHECK_EQ(sawExactBytes, 800000);             // one pair, both sides
    CHECK(namedAFile);
}

// ---------------------------------------------------------------- priority

FileEntry makeEntry(const std::string& path, int rootIndex, int64_t mtime) {
    FileEntry f;
    f.path = path;
    f.rootIndex = rootIndex;
    f.mtime = mtime;
    f.size = 100;
    return f;
}

void testPriorityAndTieBreak() {
    currentTest = "priority";
    std::vector<FileEntry> files {
        makeEntry("/second/deep/nested/copy.bin", 1, 100),
        makeEntry("/first/b.bin", 0, 300),
        makeEntry("/first/a-very-long-name.bin", 0, 200),
    };

    DupGroup g;
    g.size = 100;
    g.members = {Member {0, true}, Member {1, true}, Member {2, true}};
    std::vector<DupGroup> groups {g};

    // The directory's position decides it outright: member 0 has the oldest
    // mtime but sits in the lower priority root, so it can never win.
    resolveKeepers(groups, files, TieBreak::OldestMtime);
    CHECK_EQ(static_cast<uint64_t>(groups[0].keeper), 2);  // mtime 200 beats 300 within root 0

    resolveKeepers(groups, files, TieBreak::NewestMtime);
    CHECK_EQ(static_cast<uint64_t>(groups[0].keeper), 1);

    resolveKeepers(groups, files, TieBreak::ShortestPath);
    CHECK_EQ(static_cast<uint64_t>(groups[0].keeper), 1);

    resolveKeepers(groups, files, TieBreak::Alphabetical);
    CHECK_EQ(static_cast<uint64_t>(groups[0].keeper), 2);

    // Exactly one member is protected, whatever the rule chose.
    int unselected = 0;
    for (const auto& m : groups[0].members) {
        if (!m.selected) ++unselected;
    }
    CHECK_EQ(static_cast<uint64_t>(unselected), 1);

    // Pinning survives a later re-resolve, including one that would otherwise
    // move the keeper somewhere else.
    setKeeper(groups[0], 0);
    CHECK(groups[0].userPinned);
    resolveKeepers(groups, files, TieBreak::OldestMtime);
    CHECK_EQ(static_cast<uint64_t>(groups[0].keeper), 0);
    CHECK(!groups[0].members[0].selected);

    clearPins(groups);
    resolveKeepers(groups, files, TieBreak::OldestMtime);
    CHECK_EQ(static_cast<uint64_t>(groups[0].keeper), 2);

    // FewestSegments prefers the shallower path even when it is longer.
    resolveKeepers(groups, files, TieBreak::FewestSegments);
    CHECK(groups[0].keeper == 1 || groups[0].keeper == 2);

    const Totals t = computeTotals(groups, files);
    CHECK_EQ(t.groups, 1);
    CHECK_EQ(t.extras, 2);
    CHECK_EQ(t.reclaimable, 200);
    CHECK_EQ(t.selected, 2);
    CHECK_EQ(t.selectedBytes, 200);
}

void testSelectionOps() {
    currentTest = "selection";
    std::vector<FileEntry> files {makeEntry("/a/x", 0, 1), makeEntry("/b/x", 1, 2),
                                  makeEntry("/c/x", 2, 3)};
    DupGroup g;
    g.members = {Member {0, true}, Member {1, true}, Member {2, true}};
    g.size = 100;
    std::vector<DupGroup> groups {g};
    resolveKeepers(groups, files, TieBreak::OldestMtime);

    deselectAll(groups);
    CHECK_EQ(computeTotals(groups, files).selected, 0);

    selectAllExtras(groups);
    CHECK_EQ(computeTotals(groups, files).selected, 2);

    invertSelection(groups);
    CHECK_EQ(computeTotals(groups, files).selected, 0);

    // The keeper is never swept up by any of them.
    invertSelection(groups);
    CHECK(!groups[0].members[groups[0].keeper].selected);
}

void testPruneRemoved() {
    currentTest = "prune";
    std::vector<FileEntry> files {makeEntry("/a/x", 0, 1), makeEntry("/b/x", 1, 2),
                                  makeEntry("/c/x", 2, 3)};
    DupGroup g;
    g.members = {Member {0, false}, Member {1, true}, Member {2, true}};
    g.size = 100;
    g.keeper = 0;
    std::vector<DupGroup> groups {g};

    pruneRemoved(groups, files, {"/b/x"});
    CHECK_EQ(groups.size(), 1);
    CHECK_EQ(groups[0].members.size(), 2);
    // The keeper has to still be the keeper after the shuffle.
    CHECK(files[groups[0].members[groups[0].keeper].fileIndex].path == "/a/x");

    pruneRemoved(groups, files, {"/c/x"});
    // One copy left is not a duplicate any more, so the group goes.
    CHECK_EQ(groups.size(), 0);
}

// ---------------------------------------------------------------- scanning helpers

void testNormalizeRoots() {
    currentTest = "roots";
    const std::vector<std::string> in {"/mnt/data/", "/mnt/data/sub", "/mnt/other", "/mnt/data"};
    const std::vector<std::string> out = normalizeRoots(in);
    // The nested entry is folded in and the duplicate spelling collapses, so
    // nothing is walked twice.
    CHECK_EQ(out.size(), 2);
    CHECK(out[0] == "/mnt/data");
    CHECK(out[1] == "/mnt/other");

    // Order is priority, so it has to survive normalisation.
    const std::vector<std::string> reversed = normalizeRoots({"/mnt/other", "/mnt/data"});
    CHECK(reversed[0] == "/mnt/other");

    CHECK(pathIsUnder("/a", "/a/b"));
    CHECK(!pathIsUnder("/a", "/ab"));
}

void testHashCache() {
    currentTest = "hash-cache";
    TempDir dir;
    const std::string file = dir.path + "/cache.tsv";

    HashCache cache;
    cache.insert("/mnt/one/a.bin", 0, 300, 400, 0xABCDEF);
    CHECK(cache.save(file));

    HashCache reloaded;
    reloaded.load(file);
    uint64_t got = 0;
    CHECK(reloaded.lookup("/mnt/one/a.bin", 0, 300, 400, got));
    CHECK_EQ(got, 0xABCDEF);

    // The validation the whole cache rests on: a file whose size or mtime moved
    // is a different file as far as its contents go. Writing into a zip moves
    // both, which is exactly the case this exists for.
    CHECK(!reloaded.lookup("/mnt/one/a.bin", 0, 300, 401, got));
    CHECK(!reloaded.lookup("/mnt/one/a.bin", 0, 301, 400, got));
    CHECK(!reloaded.lookup("/mnt/two/a.bin", 0, 300, 400, got));
    // One, not two: the first stale lookup erases the entry, so the second finds
    // nothing to be stale about. A wrong path is an ordinary miss.
    CHECK_EQ(reloaded.stale(), 1);

    // A stale entry is dropped rather than left to be asked about again.
    reloaded.insert("/mnt/one/a.bin", 0, 301, 400, 0x1234);
    CHECK(reloaded.lookup("/mnt/one/a.bin", 0, 301, 400, got));
    CHECK_EQ(got, 0x1234);

    // A sampled hash is not a full hash, and must never be served as one.
    HashCache variants;
    variants.insert("/mnt/one/big.iso", 0, 900, 5, 0xFULL);
    variants.insert("/mnt/one/big.iso", 1048576, 900, 5, 0xEULL);
    CHECK(variants.lookup("/mnt/one/big.iso", 0, 900, 5, got));
    CHECK_EQ(got, 0xF);
    CHECK(variants.lookup("/mnt/one/big.iso", 1048576, 900, 5, got));
    CHECK_EQ(got, 0xE);
    CHECK(!variants.lookup("/mnt/one/big.iso", 65536, 900, 5, got));

    // Paths are the key, so a path holding a tab has to survive the format.
    HashCache odd;
    odd.insert("/mnt/a\tb/c\nd", 0, 7, 8, 0x99);
    const std::string oddFile = dir.path + "/odd.tsv";
    CHECK(odd.save(oddFile));
    HashCache oddBack;
    oddBack.load(oddFile);
    CHECK(oddBack.lookup("/mnt/a\tb/c\nd", 0, 7, 8, got));
    CHECK_EQ(got, 0x99);

    HashCache empty;
    empty.load(dir.path + "/does-not-exist.tsv");
    CHECK_EQ(empty.size(), 0);

    // A v1 file was keyed by device and inode. Those keys cannot be translated
    // without stat-ing every file they name, so it is dropped, not misread.
    const std::string v1 = dir.path + "/v1.tsv";
    {
        std::ofstream out(v1);
        out << "duplicates-ui hash cache v1\n1\t2\t300\t400\t11259375\t1700000000\n";
    }
    HashCache old;
    old.load(v1);
    CHECK_EQ(old.size(), 0);
}

void testSampledHash() {
    currentTest = "sampled-hash";
    TempDir dir;
    const std::string a = dir.path + "/a.bin";
    const std::string b = dir.path + "/b.bin";
    const uint64_t big = 4 * 1024 * 1024;

    writeFile(a, repeat('s', static_cast<size_t>(big)));
    writeFile(b, repeat('s', static_cast<size_t>(big)));

    const uint64_t sample = 16 * 4096;
    uint64_t ha = 0, hb = 0;
    bool wholeA = false, wholeB = false;
    std::atomic<uint64_t> bytes {0};

    CHECK(hashSampled(a, big, sample, nullptr, ha, wholeA, &bytes));
    CHECK(hashSampled(b, big, sample, nullptr, hb, wholeB, &bytes));
    CHECK(!wholeA);
    CHECK_EQ(ha, hb);
    // The point of the row: a 4 MB file costs the sample, not the file.
    CHECK_EQ(bytes.load(), 2 * sample);

    // A difference in the very last bytes has to be caught, because an archive
    // that grew is the common case. The final window ends on the final byte.
    std::string tail = repeat('s', static_cast<size_t>(big));
    tail[tail.size() - 1] = 'X';
    writeFile(dir.path + "/tail.bin", tail);
    uint64_t ht = 0;
    bool wholeT = false;
    CHECK(hashSampled(dir.path + "/tail.bin", big, sample, nullptr, ht, wholeT));
    CHECK(ht != ha);

    // A difference at the very start, likewise.
    std::string head = repeat('s', static_cast<size_t>(big));
    head[0] = 'X';
    writeFile(dir.path + "/head.bin", head);
    uint64_t hh = 0;
    bool wholeH = false;
    CHECK(hashSampled(dir.path + "/head.bin", big, sample, nullptr, hh, wholeH));
    CHECK(hh != ha);

    // A file inside the budget is read end to end, so the result is the ordinary
    // full hash and the caller may cache and reuse it as one.
    writeFile(dir.path + "/small.bin", repeat('q', 1000));
    uint64_t hs = 0, hfull = 0;
    bool wholeS = false;
    CHECK(hashSampled(dir.path + "/small.bin", 1000, sample, nullptr, hs, wholeS));
    CHECK(wholeS);
    CHECK(hashFull(dir.path + "/small.bin", nullptr, hfull));
    CHECK_EQ(hs, hfull);

    CHECK(!hashSampled(dir.path + "/missing.bin", big, sample, nullptr, hs, wholeS));
}

void testSampledHashInPipeline() {
    currentTest = "sampled-row";
    TempDir dir;
    const size_t big = 2 * 1024 * 1024;
    writeFile(dir.path + "/a/same.bin", repeat('y', big));
    writeFile(dir.path + "/b/same.bin", repeat('y', big));

    // Same size, and identical everywhere the sampler looks, but different in
    // between. The sampled row cannot tell them apart; the byte compare must.
    //
    // The offset is chosen to fall in the gap between two probes: with 16
    // windows of 4096 bytes over 2 MB, window 5 ends at 701781 and window 6
    // starts at 837222. This is the honest limit of a sampled hash, and the
    // reason it is a filter rather than proof.
    std::string other = repeat('y', big);
    other[750000] = 'Z';
    writeFile(dir.path + "/a/sneaky.bin", other);
    writeFile(dir.path + "/b/sneaky.bin", repeat('y', big));

    Pipeline p;
    p.threads = 2;
    p.rules = {
        Rule {true, RuleKind::MinSize, {}, 1},
        Rule {true, RuleKind::SampledHash, {}, 16 * 4096},
        Rule {true, RuleKind::ExactBytes, {}, 0},
    };
    const ScanOutcome proven = scanPipeline(dir.path, p, ScopeFilters {});
    // All four files are the same size and same sample, so the byte compare is
    // what separates the odd one out: three identical, one alone.
    CHECK_EQ(proven.groups.size(), 1);
    const int g = findGroup(proven, "/a/same.bin");
    CHECK(g >= 0);
    if (g >= 0) CHECK_EQ(proven.groups[g].members.size(), 3);

    // Without the byte compare the sampled row alone calls all four a match,
    // which is exactly why the interface objects to that pipeline.
    Pipeline weak = p;
    weak.rules.pop_back();
    const ScanOutcome guessed = scanPipeline(dir.path, weak, ScopeFilters {});
    CHECK_EQ(guessed.groups.size(), 1);
    if (!guessed.groups.empty()) CHECK_EQ(guessed.groups[0].members.size(), 4);

    // The floor keeps a sample from shrinking below one page per window.
    Pipeline tiny;
    tiny.rules = {Rule {true, RuleKind::SampledHash, {}, 10}};
    CHECK_EQ(compilePipeline(tiny).splits[0].number,
             static_cast<uint64_t>(kSampleWindows) * 4096);
}

void testUniquesReport() {
    currentTest = "uniques";
    TempDir dir;
    writeFile(dir.path + "/a/pair.bin", repeat('p', 5000));
    writeFile(dir.path + "/b/pair.bin", repeat('p', 5000));
    writeFile(dir.path + "/a/alone.bin", repeat('a', 5000));   // unique by content
    writeFile(dir.path + "/b/odd.bin", repeat('o', 777));      // unique by size

    Pipeline p;
    p.threads = 2;
    p.rules = {
        Rule {true, RuleKind::MinSize, {}, 1},
        Rule {true, RuleKind::HeadBytes, {}, 512},
        Rule {true, RuleKind::ExactBytes, {}, 0},
    };

    const ScanOutcome dupes = scanPipeline(dir.path, p, ScopeFilters {});
    CHECK_EQ(dupes.groups.size(), 1);

    p.report = ReportMode::Uniques;
    const ScanOutcome alone = scanPipeline(dir.path, p, ScopeFilters {});

    // A file dropped alone by any row is unique, whether it fell out at the size
    // row or survived to the byte compare and matched nothing.
    CHECK_EQ(alone.groups.size(), 2);
    CHECK(findGroup(alone, "/a/alone.bin") >= 0);
    CHECK(findGroup(alone, "/b/odd.bin") >= 0);
    CHECK(findGroup(alone, "/a/pair.bin") < 0);
    for (const auto& g : alone.groups) {
        CHECK(g.unique);
        CHECK_EQ(g.members.size(), 1);
    }

    // Duplicates and uniques partition the input: nothing is in both, nothing is
    // in neither.
    CHECK_EQ(dupes.files.size(), 4);
    CHECK_EQ(alone.groups.size() + 2 * dupes.groups.size(), alone.files.size());

    // The one file is selectable, because a uniques run is there to be acted on,
    // and it reclaims nothing, because removing it removes the data.
    const Totals t = computeTotals(alone.groups, alone.files);
    CHECK_EQ(t.uniques, 2);
    CHECK_EQ(t.extras, 0);
    CHECK_EQ(t.reclaimable, 0);
    CHECK_EQ(t.selected, 2);

    // Selection helpers must not treat the only copy as a keeper to protect.
    std::vector<DupGroup> groups = alone.groups;
    deselectAll(groups);
    CHECK_EQ(computeTotals(groups, alone.files).selected, 0);
    selectAllExtras(groups);
    CHECK_EQ(computeTotals(groups, alone.files).selected, 2);
    invertSelection(groups);
    CHECK_EQ(computeTotals(groups, alone.files).selected, 0);

    // And resolveKeepers, which exists to keep exactly one copy safe, must not
    // quietly clear the selection when there is no second copy to fall back on.
    selectAllExtras(groups);
    resolveKeepers(groups, alone.files, TieBreak::OldestMtime);
    CHECK_EQ(computeTotals(groups, alone.files).selected, 2);
}

// ---------------------------------------------------------------- actions

void waitFor(ActionQueue& q) {
    while (q.running()) ::usleep(2000);
}

void testQuarantinePaths() {
    currentTest = "quarantine-paths";
    CHECK(quarantineDest("/q", "/home/declan/a.txt") == "/q/home/declan/a.txt");

    TempDir dir;
    writeFile(dir.path + "/taken.txt", "x");
    // Something is already there, so the name is stepped rather than overwritten.
    CHECK(uniquePath(dir.path + "/taken.txt") == dir.path + "/taken (2).txt");
    CHECK(uniquePath(dir.path + "/free.txt") == dir.path + "/free.txt");

    writeFile(dir.path + "/taken (2).txt", "x");
    CHECK(uniquePath(dir.path + "/taken.txt") == dir.path + "/taken (3).txt");
}

ActionItem itemFor(const std::string& path, const std::string& keeper) {
    struct stat st {};
    struct stat ks {};
    ::lstat(path.c_str(), &st);
    ::lstat(keeper.c_str(), &ks);

    ActionItem item;
    item.path = path;
    item.keeper = keeper;
    item.size = static_cast<uint64_t>(st.st_size);
    item.mtime = static_cast<int64_t>(st.st_mtime);
    item.keeperSize = static_cast<uint64_t>(ks.st_size);
    item.keeperMtime = static_cast<int64_t>(ks.st_mtime);
    return item;
}

void testQuarantineAndRestore() {
    currentTest = "quarantine-run";
    TempDir dir;
    const std::string keeper = dir.path + "/a/keep.txt";
    const std::string extra = dir.path + "/b/extra.txt";
    const std::string quarantine = dir.path + "/quarantine";
    writeFile(keeper, "payload");
    writeFile(extra, "payload");

    ActionQueue q;
    q.start({itemFor(extra, keeper)}, ActionKind::Quarantine, quarantine, nullptr);
    waitFor(q);

    RunSummary summary;
    CHECK(q.takeSummary(summary));
    CHECK_EQ(summary.done, 1);
    CHECK_EQ(summary.failed, 0);
    CHECK(!exists(extra));
    CHECK(exists(quarantine + extra));  // the original absolute path is mirrored
    CHECK(exists(keeper));              // the copy being kept is untouched
    CHECK_EQ(summary.removed.size(), 1);

    std::vector<RunEntry> runs = loadRuns(quarantine);
    CHECK_EQ(runs.size(), 1);
    if (runs.empty()) return;
    CHECK_EQ(runs[0].files, 1);
    CHECK(runs[0].kind == ActionKind::Quarantine);

    const RestoreResult r = restoreRun(runs[0]);
    CHECK_EQ(r.restored, 1);
    CHECK_EQ(r.skipped, 0);
    CHECK(exists(extra));

    // Restoring again must not overwrite whatever now lives at the original
    // path, which is the entire point of the feature.
    writeFile(quarantine + extra, "payload");
    const RestoreResult again = restoreRun(runs[0]);
    CHECK_EQ(again.restored, 0);
    CHECK_EQ(again.skipped, 1);
}

void testDeleteRun() {
    currentTest = "delete-run";
    TempDir dir;
    const std::string keeper = dir.path + "/a/keep.txt";
    const std::string extra = dir.path + "/b/extra.txt";
    writeFile(keeper, "payload");
    writeFile(extra, "payload");

    ActionQueue q;
    q.start({itemFor(extra, keeper)}, ActionKind::Delete, {}, nullptr);
    waitFor(q);

    RunSummary summary;
    CHECK(q.takeSummary(summary));
    CHECK_EQ(summary.done, 1);
    CHECK(!exists(extra));
    CHECK(exists(keeper));

    // Delete runs are recorded even though they cannot be undone.
    std::error_code ec;
    CHECK(fs::exists(summary.manifest, ec));
    fs::remove(summary.manifest, ec);
}

void testVerifyBeforeActing() {
    currentTest = "verify";
    TempDir dir;
    const std::string keeper = dir.path + "/a/keep.txt";
    const std::string extra = dir.path + "/b/extra.txt";
    writeFile(keeper, "payload");
    writeFile(extra, "payload");

    ActionItem item = itemFor(extra, keeper);
    item.mtime -= 100;  // pretend the scan saw an older version

    ActionQueue q;
    q.start({item}, ActionKind::Delete, {}, nullptr);
    waitFor(q);

    RunSummary summary;
    CHECK(q.takeSummary(summary));
    CHECK_EQ(summary.done, 0);
    CHECK_EQ(summary.failed, 1);
    // A file that moved under the scan is left exactly where it is.
    CHECK(exists(extra));

    std::error_code ec;
    fs::remove(summary.manifest, ec);

    // The same protection applies to the copy that is supposed to survive.
    ActionItem keeperMoved = itemFor(extra, keeper);
    keeperMoved.keeperSize += 1;
    ActionQueue q2;
    q2.start({keeperMoved}, ActionKind::Delete, {}, nullptr);
    waitFor(q2);
    RunSummary s2;
    CHECK(q2.takeSummary(s2));
    CHECK_EQ(s2.failed, 1);
    CHECK(exists(extra));
    fs::remove(s2.manifest, ec);

    std::string err;
    CHECK(verifyUnchanged(keeper, 7, itemFor(keeper, keeper).mtime, err));
    CHECK(!verifyUnchanged(dir.path + "/nope", 1, 1, err));
}

// ---------------------------------------------------------------- session

void testSessionRoundTrip() {
    currentTest = "session";
    SessionData d;
    d.roots = {"/mnt/one", "/home/user/pictures with spaces"};
    d.scope.includeHidden = true;
    d.scope.collapseHardlinks = false;
    d.pipeline.rules = {
        Rule {true, RuleKind::MinSize, {}, 4096},
        Rule {true, RuleKind::Glob, "*.7z", 0},
        Rule {false, RuleKind::Regex, "\\.part[0-9]+$", 0},
        Rule {true, RuleKind::SameName, {}, 0},
        Rule {true, RuleKind::HeadBytes, {}, 8192},
        Rule {true, RuleKind::FullHash, {}, 0},
    };
    d.pipeline.combine = PatternCombine::All;
    d.pipeline.select = PatternSelect::Include;
    d.pipeline.threads = 12;
    d.tie = TieBreak::ShortestPath;
    d.quarantineRoot = "/mnt/quarantine";
    d.action = ActionKind::Delete;
    d.sort = GroupSort::MembersDesc;
    d.filter.text = "holiday";
    d.filter.minSize = 1024;
    d.filter.minMembers = 3;
    d.showLog = false;

    const SessionData back = parseSession(serializeSession(d));
    CHECK(back.roots == d.roots);
    CHECK(back.scope.includeHidden);
    CHECK(!back.scope.collapseHardlinks);
    CHECK_EQ(back.pipeline.rules.size(), d.pipeline.rules.size());
    // Order is the pipeline, so it has to survive the round trip exactly.
    for (size_t i = 0; i < back.pipeline.rules.size() && i < d.pipeline.rules.size(); ++i) {
        CHECK(back.pipeline.rules[i].kind == d.pipeline.rules[i].kind);
        CHECK(back.pipeline.rules[i].enabled == d.pipeline.rules[i].enabled);
        CHECK(back.pipeline.rules[i].pattern == d.pipeline.rules[i].pattern);
        CHECK_EQ(back.pipeline.rules[i].number, d.pipeline.rules[i].number);
    }
    CHECK(back.pipeline.combine == PatternCombine::All);
    CHECK(back.pipeline.select == PatternSelect::Include);
    CHECK_EQ(static_cast<uint64_t>(back.pipeline.threads), 12);
    CHECK(back.tie == TieBreak::ShortestPath);
    CHECK(back.quarantineRoot == d.quarantineRoot);
    CHECK(back.action == ActionKind::Delete);
    CHECK(back.sort == GroupSort::MembersDesc);
    CHECK(back.filter.text == "holiday");
    CHECK_EQ(back.filter.minSize, 1024);
    CHECK_EQ(static_cast<uint64_t>(back.filter.minMembers), 3);
    CHECK(!back.showLog);

    // An empty rule list is a real choice, not a reason to fall back to the
    // default one.
    SessionData bare;
    bare.pipeline.rules.clear();
    CHECK(parseSession(serializeSession(bare)).pipeline.rules.empty());

    // A path holding a tab or a newline has to survive a tab separated format.
    SessionData weird;
    weird.roots = {"/a\tb", "/c\nd", "/e\\f"};
    CHECK(parseSession(serializeSession(weird)).roots == weird.roots);

    // Anything unrecognised gives defaults rather than nonsense.
    CHECK(parseSession("").roots.empty());
    CHECK(parseSession("v99\nroot\t/x\n").roots.empty());
}

void testSessionV1Migration() {
    currentTest = "session-v1";
    // Exactly what the previous version wrote: a size floor and exclude globs in
    // the scope record, and the fixed cascade in the stage record.
    const std::string v1 =
        "v1\n"
        "root\t/mnt/one\n"
        "scope\t4096\t1\t0\t*.tmp,*.part\n"
        "stage\t1\t0\t1\t8192\t1\t0\t12\n"
        "tie\t2\n"
        "quar\t/mnt/quarantine\n"
        "act\t0\n"
        "view\t2\t1024\t3\t0\tholiday\n";

    const SessionData d = parseSession(v1);
    CHECK(d.roots.size() == 1);
    CHECK(d.scope.includeHidden);
    CHECK(!d.scope.collapseHardlinks);
    CHECK_EQ(static_cast<uint64_t>(d.pipeline.threads), 12);
    CHECK(d.pipeline.select == PatternSelect::Exclude);

    // The old fixed order, rebuilt: floor, the two globs, same filename, then
    // the three content rows with exact compare switched off as it was.
    CHECK_EQ(d.pipeline.rules.size(), 7);
    if (d.pipeline.rules.size() == 7) {
        CHECK(d.pipeline.rules[0].kind == RuleKind::MinSize);
        CHECK_EQ(d.pipeline.rules[0].number, 4096);
        CHECK(d.pipeline.rules[1].kind == RuleKind::Glob);
        CHECK(d.pipeline.rules[1].pattern == "*.tmp");
        CHECK(d.pipeline.rules[2].pattern == "*.part");
        CHECK(d.pipeline.rules[3].kind == RuleKind::SameName);
        CHECK(d.pipeline.rules[4].kind == RuleKind::HeadBytes);
        CHECK_EQ(d.pipeline.rules[4].number, 8192);
        CHECK(d.pipeline.rules[5].kind == RuleKind::FullHash);
        CHECK(d.pipeline.rules[6].kind == RuleKind::ExactBytes);
        CHECK(!d.pipeline.rules[6].enabled);
    }
    CHECK(d.tie == TieBreak::ShortestPath);
    CHECK(d.action == ActionKind::Delete);
}

void testFormatting() {
    currentTest = "formatting";
    CHECK(formatSize(512) == "512 B");
    CHECK(formatSize(2048) == "2.0 KB");
    CHECK(formatCount(0) == "0");
    CHECK(formatCount(999) == "999");
    CHECK(formatCount(1000) == "1,000");
    CHECK(formatCount(1234567) == "1,234,567");
    CHECK(formatDuration(0.0077) == "8 ms");
    CHECK(formatDuration(42.5) == "42.5 s");
    CHECK(formatDuration(83.0) == "1m 23s");
    CHECK(normalizePath("  /a/b/  ") == "/a/b");
    CHECK(escapeField("a\tb\nc\\d") == "a\\tb\\nc\\\\d");
    CHECK(unescapeField(escapeField("a\tb\nc\\d")) == "a\tb\nc\\d");
}

}  // namespace

int main() {
    // Delete runs are recorded in the state directory. Point every XDG path at
    // a temporary one so running the suite never touches the real user's files.
    TempDir xdg;
    ::setenv("XDG_STATE_HOME", (xdg.path + "/state").c_str(), 1);
    ::setenv("XDG_CACHE_HOME", (xdg.path + "/cache").c_str(), 1);

    testHash();
    testCascade();
    testHeadIsFullShortcut();
    testExactCompareSplitsBucket();
    testHardlinkCollapseOff();
    testExcludeGlobsAndHidden();
    testIncludeAndCombineModes();
    testRegexRules();
    testDirectoryPruning();
    testRuleOrderAndCompile();
    testSizeBounds();
    testLogBuffer();
    testChunkProgressCallbacks();
    testCascadeReportsBytesAndCurrentFile();
    testPriorityAndTieBreak();
    testSelectionOps();
    testPruneRemoved();
    testNormalizeRoots();
    testHashCache();
    testSampledHash();
    testSampledHashInPipeline();
    testUniquesReport();
    testQuarantinePaths();
    testQuarantineAndRestore();
    testDeleteRun();
    testVerifyBeforeActing();
    testSessionRoundTrip();
    testSessionV1Migration();
    testFormatting();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
