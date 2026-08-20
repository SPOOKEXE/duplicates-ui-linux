#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "runs.h"

// One planned removal, carrying everything needed to prove at apply time that
// the file is still exactly what the scan saw.
struct ActionItem {
    std::string path;    // the extra, the file that goes
    std::string keeper;  // the copy that stays, verified before the extra is touched
    uint64_t size = 0;
    int64_t mtime = 0;
    uint64_t keeperSize = 0;
    int64_t keeperMtime = 0;
    uint64_t hash = 0;
};

enum class ActionState { Pending, Done, Failed };

struct ActionRow {
    ActionItem item;
    ActionState state = ActionState::Pending;
    std::string dest;  // where a quarantined file landed
    std::string error;
};

// Live view of a run, cheap enough to poll every frame: failures are the only
// rows carried, and they are capped.
struct ActionProgress {
    bool running = false;
    ActionKind kind = ActionKind::Quarantine;
    size_t total = 0;
    size_t done = 0;
    size_t failed = 0;
    uint64_t bytes = 0;
    std::string current;
    std::vector<ActionRow> failures;
};

struct RunSummary {
    ActionKind kind = ActionKind::Quarantine;
    size_t done = 0;
    size_t failed = 0;
    uint64_t bytes = 0;
    bool cancelled = false;
    std::string manifest;
    // Paths that really are gone, so the results list can be pruned without a
    // rescan. Only successful rows appear here.
    std::vector<std::string> removed;
};

// The quarantine destination mirrors the original absolute path under the
// quarantine root, so /home/declan/a.txt becomes <root>/home/declan/a.txt and
// two files with the same name never collide.
std::string quarantineDest(const std::string& root, const std::string& original);

// The same path, with " (2)", " (3)" and so on inserted before the extension
// until nothing is in the way.
std::string uniquePath(const std::string& desired);

// Confirms a file is still the one the scan measured. Anything else, including
// it having become a directory or a symlink, fails.
bool verifyUnchanged(const std::string& path, uint64_t size, int64_t mtime, std::string& err);

// Applies the selected removals, one at a time, on a background thread.
//
// Sequential on purpose: these operations are metadata-fast apart from a
// cross-device quarantine copy, and a half-parallel destructive run is much
// harder to reason about after a cancel.
class ActionQueue {
public:
    ActionQueue() = default;
    ~ActionQueue();

    ActionQueue(const ActionQueue&) = delete;
    ActionQueue& operator=(const ActionQueue&) = delete;

    void start(std::vector<ActionItem> items, ActionKind kind, std::string quarantineRoot);
    void cancel();
    bool running() const;

    ActionProgress progress() const;

    // True on the single call after a run finishes.
    bool takeSummary(RunSummary& out);

private:
    void run(std::vector<ActionItem> items, ActionKind kind, std::string quarantineRoot);

    mutable std::mutex mutex_;
    std::thread thread_;
    std::atomic<bool> cancel_ {false};
    std::atomic<bool> running_ {false};

    ActionProgress progress_;
    bool summaryReady_ = false;
    RunSummary summary_;
};
