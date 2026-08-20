// One binary, one assert macro, no framework: the same spirit as the rest of
// the project. Every test builds its own fixture tree in a temporary directory
// and removes it afterwards, so the suite is safe to run anywhere.
#include <sys/stat.h>
#include <unistd.h>

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

ScanOutcome scanFixture(const std::string& base, StageSettings stages, ScopeFilters scope,
                        TieBreak tie = TieBreak::OldestMtime) {
    ScanOutcome out;
    WalkHooks hooks;
    out.files = walkRoots({base + "/a", base + "/b"}, scope, hooks, out.walk);
    if (scope.collapseHardlinks) collapseHardlinks(out.files, tie, out.walk);

    CascadeHooks ch;
    ch.threads = 2;
    out.groups = runCascade(out.files, stages, ch);
    resolveKeepers(out.groups, out.files, tie);
    return out;
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

    StageSettings stages;
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

    StageSettings stages;
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

    StageSettings stages;
    stages.headBytes = false;
    stages.fullHash = false;
    stages.exactCompare = true;
    ScopeFilters scope;

    const ScanOutcome o = scanFixture(dir.path, stages, scope);
    CHECK_EQ(o.groups.size(), 0);

    // Without it, size alone would have called them duplicates, which is exactly
    // why the warning exists in the interface.
    StageSettings unsafe = stages;
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

    StageSettings stages;
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

    StageSettings stages;
    ScopeFilters scope;
    scope.excludeGlobs = "*.tmp";
    const ScanOutcome o = scanFixture(dir.path, stages, scope);
    CHECK_EQ(o.groups.size(), 1);
    CHECK_EQ(o.walk.skippedExcluded, 2);
    CHECK_EQ(o.walk.skippedHidden, 1);

    scope.excludeGlobs.clear();
    scope.includeHidden = true;
    const ScanOutcome withHidden = scanFixture(dir.path, stages, scope);
    // keep.bin x2 plus the hidden copy is one group of three, and the two .tmp
    // files are now a group of their own.
    CHECK_EQ(withHidden.groups.size(), 2);
    const int g = findGroup(withHidden, "/a/keep.bin");
    CHECK(g >= 0);
    if (g >= 0) CHECK_EQ(withHidden.groups[g].members.size(), 3);
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

    CHECK(matchesAnyGlob("/x/y/file.tmp", {"*.tmp"}));
    CHECK(matchesAnyGlob("/mnt/scratch/a", {"/mnt/scratch/*"}));
    CHECK(!matchesAnyGlob("/x/y/file.txt", {"*.tmp"}));
    CHECK(pathIsUnder("/a", "/a/b"));
    CHECK(!pathIsUnder("/a", "/ab"));
}

void testHashCache() {
    currentTest = "hash-cache";
    TempDir dir;
    const std::string file = dir.path + "/cache.tsv";

    HashCache cache;
    cache.insert(1, 2, 300, 400, 0xABCDEF);
    CHECK(cache.save(file));

    HashCache reloaded;
    reloaded.load(file);
    uint64_t got = 0;
    CHECK(reloaded.lookup(1, 2, 300, 400, got));
    CHECK_EQ(got, 0xABCDEF);

    // Any change to the file's identity misses, which is the whole invalidation
    // strategy: there is nothing to get wrong.
    CHECK(!reloaded.lookup(1, 2, 300, 401, got));
    CHECK(!reloaded.lookup(1, 2, 301, 400, got));
    CHECK(!reloaded.lookup(1, 3, 300, 400, got));

    HashCache empty;
    empty.load(dir.path + "/does-not-exist.tsv");
    CHECK_EQ(empty.size(), 0);
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
    q.start({itemFor(extra, keeper)}, ActionKind::Quarantine, quarantine);
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
    q.start({itemFor(extra, keeper)}, ActionKind::Delete, {});
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
    q.start({item}, ActionKind::Delete, {});
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
    q2.start({keeperMoved}, ActionKind::Delete, {});
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
    d.scope.minSize = 4096;
    d.scope.includeHidden = true;
    d.scope.collapseHardlinks = false;
    d.scope.excludeGlobs = "*.tmp,*.part";
    d.stages.sameName = true;
    d.stages.headSize = 8192;
    d.stages.exactCompare = false;
    d.stages.hashThreads = 12;
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
    CHECK_EQ(back.scope.minSize, 4096);
    CHECK(back.scope.includeHidden);
    CHECK(!back.scope.collapseHardlinks);
    CHECK(back.scope.excludeGlobs == d.scope.excludeGlobs);
    CHECK(back.stages.sameName);
    CHECK_EQ(back.stages.headSize, 8192);
    CHECK(!back.stages.exactCompare);
    CHECK_EQ(static_cast<uint64_t>(back.stages.hashThreads), 12);
    CHECK(back.tie == TieBreak::ShortestPath);
    CHECK(back.quarantineRoot == d.quarantineRoot);
    CHECK(back.action == ActionKind::Delete);
    CHECK(back.sort == GroupSort::MembersDesc);
    CHECK(back.filter.text == "holiday");
    CHECK_EQ(back.filter.minSize, 1024);
    CHECK_EQ(static_cast<uint64_t>(back.filter.minMembers), 3);
    CHECK(!back.showLog);

    // A path holding a tab or a newline has to survive a tab separated format.
    SessionData weird;
    weird.roots = {"/a\tb", "/c\nd", "/e\\f"};
    CHECK(parseSession(serializeSession(weird)).roots == weird.roots);

    // Anything unrecognised gives defaults rather than nonsense.
    CHECK(parseSession("").roots.empty());
    CHECK(parseSession("v99\nroot\t/x\n").roots.empty());
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
    testPriorityAndTieBreak();
    testSelectionOps();
    testPruneRemoved();
    testNormalizeRoots();
    testHashCache();
    testQuarantinePaths();
    testQuarantineAndRestore();
    testDeleteRun();
    testVerifyBeforeActing();
    testSessionRoundTrip();
    testFormatting();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
