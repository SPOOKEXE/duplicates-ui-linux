#include "scan_engine.h"

#include <chrono>

#include "groups.h"
#include "hash_cache.h"

namespace {

// The log is a rolling window: a scan of a broken disk can produce an error per
// file, and keeping all of them would be its own memory problem.
constexpr size_t kMaxLogLines = 500;

}  // namespace

ScanEngine::~ScanEngine() {
    cancel();
    if (thread_.joinable()) thread_.join();
}

void ScanEngine::start(std::vector<std::string> roots, ScopeFilters scope, StageSettings stages,
                       TieBreak tie, HashCache* cache) {
    if (running_.load()) return;
    if (thread_.joinable()) thread_.join();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        resultsReady_ = false;
        files_.clear();
        groups_.clear();
        stats_ = ScanStats {};
        progress_ = CascadeProgress {};
    }

    cancel_.store(false);
    running_.store(true);
    thread_ = std::thread(&ScanEngine::run, this, std::move(roots), std::move(scope), stages, tie,
                          cache);
}

void ScanEngine::cancel() { cancel_.store(true); }

bool ScanEngine::running() const { return running_.load(); }

CascadeProgress ScanEngine::progress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return progress_;
}

std::vector<std::string> ScanEngine::log() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {log_.begin(), log_.end()};
}

void ScanEngine::clearLog() {
    std::lock_guard<std::mutex> lock(mutex_);
    log_.clear();
}

bool ScanEngine::takeResults(std::vector<FileEntry>& files, std::vector<DupGroup>& groups,
                             ScanStats& stats) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!resultsReady_) return false;
    files = std::move(files_);
    groups = std::move(groups_);
    stats = stats_;
    files_.clear();
    groups_.clear();
    resultsReady_ = false;
    return true;
}

void ScanEngine::appendLog(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    log_.push_back(line);
    while (log_.size() > kMaxLogLines) log_.pop_front();
}

void ScanEngine::run(std::vector<std::string> roots, ScopeFilters scope, StageSettings stages,
                     TieBreak tie, HashCache* cache) {
    const auto started = std::chrono::steady_clock::now();
    ScanStats stats;
    std::atomic<uint64_t> errorCount {0};

    WalkHooks walkHooks;
    walkHooks.cancel = &cancel_;
    walkHooks.onError = [&](const std::string& m) {
        errorCount.fetch_add(1);
        appendLog(m);
    };
    walkHooks.onProgress = [&](uint64_t seen, const std::string& dir) {
        std::lock_guard<std::mutex> lock(mutex_);
        progress_.stage = Stage::Walking;
        progress_.done = seen;
        progress_.total = 0;  // unknowable until the walk is over
        progress_.candidates = seen;
        (void)dir;  // no denominator exists yet, so the count is the whole story
    };

    std::vector<FileEntry> files = walkRoots(roots, scope, walkHooks, stats.walk);

    if (scope.collapseHardlinks && !cancel_.load()) {
        collapseHardlinks(files, tie, stats.walk);
    }

    CascadeHooks hooks;
    hooks.cancel = &cancel_;
    hooks.cache = cache;
    hooks.threads = stages.hashThreads;
    hooks.onError = [&](const std::string& m) {
        errorCount.fetch_add(1);
        appendLog(m);
    };
    hooks.onProgress = [&](const CascadeProgress& p) {
        std::lock_guard<std::mutex> lock(mutex_);
        // candidates only ever shrinks, and a stage reports it as zero while it
        // is still working, so keep the last real figure for the UI.
        const uint64_t keep = progress_.candidates;
        progress_ = p;
        if (p.candidates == 0 && p.stage != Stage::Done) progress_.candidates = keep;
    };

    std::vector<DupGroup> groups = runCascade(files, stages, hooks);
    const bool cancelled = cancel_.load();

    if (!cancelled) {
        resolveKeepers(groups, files, tie);
        sortGroups(groups, files, GroupSort::ReclaimableDesc);
    }

    const Totals totals = computeTotals(groups, files);
    stats.groups = totals.groups;
    stats.extras = totals.extras;
    stats.reclaimable = totals.reclaimable;
    stats.errors = errorCount.load();
    stats.cancelled = cancelled;
    stats.cacheHits = cache ? cache->hits() : 0;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats.bytesRead = progress_.bytesRead;
        stats.candidates = progress_.candidates;
        files_ = std::move(files);
        groups_ = std::move(groups);
        stats_ = stats;
        resultsReady_ = true;
        progress_.stage = cancelled ? Stage::Cancelled : Stage::Done;
        progress_.done = 0;
        progress_.total = 0;
    }
    running_.store(false);
}
