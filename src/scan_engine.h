#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cascade.h"
#include "dupes.h"
#include "scanner.h"

class HashCache;

// Everything worth reporting once a scan finishes.
struct ScanStats {
    WalkStats walk;
    uint64_t candidates = 0;  // files that survived the size stage
    uint64_t groups = 0;
    uint64_t extras = 0;
    uint64_t reclaimable = 0;
    uint64_t bytesRead = 0;
    double seconds = 0.0;
    uint64_t cacheHits = 0;
    uint64_t errors = 0;
    bool cancelled = false;
};

// Owns the scan: one background thread that walks, collapses hardlinks and runs
// the cascade, with a pool underneath it for the hashing stages.
//
// The contract is the one JobQueue uses in rsync-ui: every accessor is safe to
// call from the UI thread, all shared state sits behind one mutex, and the UI
// gets copies rather than references. Results are handed over exactly once, so
// the frame loop is never copying a hundred thousand groups.
class ScanEngine {
public:
    ScanEngine() = default;
    ~ScanEngine();

    ScanEngine(const ScanEngine&) = delete;
    ScanEngine& operator=(const ScanEngine&) = delete;

    // Ignored while a scan is already running.
    void start(std::vector<std::string> roots, ScopeFilters scope, StageSettings stages,
               TieBreak tie, HashCache* cache);

    void cancel();
    bool running() const;

    CascadeProgress progress() const;
    std::vector<std::string> log() const;
    void clearLog();

    // True on the single call after a scan finishes, moving the results out.
    bool takeResults(std::vector<FileEntry>& files, std::vector<DupGroup>& groups,
                     ScanStats& stats);

private:
    void run(std::vector<std::string> roots, ScopeFilters scope, StageSettings stages,
             TieBreak tie, HashCache* cache);
    void appendLog(const std::string& line);

    mutable std::mutex mutex_;
    std::thread thread_;
    std::atomic<bool> cancel_ {false};
    std::atomic<bool> running_ {false};

    CascadeProgress progress_;
    std::deque<std::string> log_;

    bool resultsReady_ = false;
    std::vector<FileEntry> files_;
    std::vector<DupGroup> groups_;
    ScanStats stats_;
};
