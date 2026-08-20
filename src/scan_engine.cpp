#include "scan_engine.h"

#include <chrono>

#include "groups.h"
#include "hash_cache.h"
#include "log.h"
#include "util.h"

ScanEngine::~ScanEngine() {
    cancel();
    if (thread_.joinable()) thread_.join();
}

void ScanEngine::start(std::vector<std::string> roots, ScopeFilters scope, Pipeline pipeline,
                       TieBreak tie, HashCache* cache, Log* log) {
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
    thread_ = std::thread(&ScanEngine::run, this, std::move(roots), std::move(scope),
                          std::move(pipeline), tie, cache, log);
}

void ScanEngine::cancel() { cancel_.store(true); }

bool ScanEngine::running() const { return running_.load(); }

CascadeProgress ScanEngine::progress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return progress_;
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

void ScanEngine::run(std::vector<std::string> roots, ScopeFilters scope, Pipeline pipeline,
                     TieBreak tie, HashCache* cache, Log* log) {
    const auto started = std::chrono::steady_clock::now();
    ScanStats stats;
    std::atomic<uint64_t> errorCount {0};

    const auto note = [&](const std::string& m) {
        if (log) log->info(m);
    };
    const auto fail = [&](const std::string& m) {
        errorCount.fetch_add(1);
        if (log) log->error(m);
    };

    const CompiledPipeline pipe = compilePipeline(pipeline);

    note("scan started: " + formatCount(roots.size()) + " input director" +
         (roots.size() == 1 ? "y" : "ies"));
    for (size_t i = 0; i < roots.size(); ++i) {
        note("  " + std::to_string(i + 1) + ". " + roots[i]);
    }
    note("pipeline: " + describePipeline(pipe));
    note(std::string("scope: ") + (scope.includeHidden ? "hidden files included" : "hidden files skipped") +
         ", " + (scope.collapseHardlinks ? "hardlinks folded" : "hardlinks kept apart") +
         ", keeping the " + tieBreakName(tie) + " on a tie, " + std::to_string(pipe.threads) +
         " hash thread(s)");
    // A rule that could not be built is the user's rule going missing, so it is
    // said out loud rather than left for them to notice in the results.
    for (const auto& problem : pipe.problems) {
        if (log) log->warn(problem);
    }

    WalkHooks walkHooks;
    walkHooks.cancel = &cancel_;
    walkHooks.onError = fail;
    walkHooks.onNote = note;
    walkHooks.onProgress = [&](uint64_t seen, const std::string& dir) {
        std::lock_guard<std::mutex> lock(mutex_);
        progress_.stage = Stage::Walking;
        progress_.done = seen;
        progress_.total = 0;  // unknowable until the walk is over
        progress_.candidates = seen;
        (void)dir;  // no denominator exists yet, so the count is the whole story
    };

    const auto walkStarted = std::chrono::steady_clock::now();
    std::vector<FileEntry> files = walkRoots(roots, scope, pipe, walkHooks, stats.walk);
    const double walkSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - walkStarted).count();

    const WalkStats& w = stats.walk;
    note("walk finished: " + formatCount(w.filesSeen) + " file(s), " + formatSize(w.bytesSeen) +
         ", in " + formatDuration(walkSeconds));
    note("skipped: " + formatCount(w.skippedHidden) + " hidden, " + formatCount(w.skippedSmall) +
         " below the floor, " + formatCount(w.skippedLarge) + " above the ceiling, " +
         formatCount(w.skippedSymlink) + " symlink(s), " + formatCount(w.skippedFiltered) +
         " filtered out, " + formatCount(w.prunedDirs) + " pruned director(ies), " +
         formatCount(w.dirErrors) + " unreadable director(ies)");

    if (scope.collapseHardlinks && !cancel_.load()) {
        collapseHardlinks(files, tie, stats.walk, &walkHooks);
        if (stats.walk.collapsedLinks > 0) {
            note("hardlinks: " + formatCount(stats.walk.collapsedLinks) +
                 " path(s) folded into their representative, leaving " + formatCount(files.size()) +
                 " file(s)");
        }
    }

    CascadeHooks hooks;
    hooks.cancel = &cancel_;
    hooks.cache = cache;
    hooks.onError = fail;
    hooks.onNote = note;
    hooks.onProgress = [&](const CascadeProgress& p) {
        std::lock_guard<std::mutex> lock(mutex_);
        // candidates only ever shrinks, and a row reports it as zero while it
        // is still working, so keep the last real figure for the UI.
        const uint64_t keep = progress_.candidates;
        progress_ = p;
        if (p.candidates == 0 && p.stage != Stage::Done) progress_.candidates = keep;
    };

    std::vector<DupGroup> groups = runCascade(files, pipe, hooks);
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

    if (cancelled) {
        if (log) log->warn("scan cancelled after " + formatDuration(stats.seconds));
    } else {
        note("scan finished: " + formatCount(stats.groups) + " group(s), " +
             formatCount(stats.extras) + " extra(s), " + formatSize(stats.reclaimable) +
             " reclaimable, read " + formatSize(stats.bytesRead) + " in " +
             formatDuration(stats.seconds) + ", " + formatCount(stats.errors) + " error(s)");
    }
    running_.store(false);
}
