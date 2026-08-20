#include "actions.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>

#include "log.h"
#include "util.h"

namespace fs = std::filesystem;

namespace {

// Enough failures to see the pattern, not enough to turn a bad run into a
// memory problem.
constexpr size_t kMaxFailuresKept = 200;

bool statFile(const std::string& path, struct stat& st) {
    return ::lstat(path.c_str(), &st) == 0;
}

}  // namespace

std::string quarantineDest(const std::string& root, const std::string& original) {
    if (original.empty() || original[0] != '/') return root + "/" + original;
    return root + original;
}

std::string uniquePath(const std::string& desired) {
    struct stat st {};
    if (::lstat(desired.c_str(), &st) != 0) return desired;

    const fs::path p(desired);
    const std::string parent = p.parent_path().string();
    const std::string stem = p.stem().string();
    const std::string ext = p.extension().string();

    for (int n = 2; n < 10000; ++n) {
        const std::string candidate =
            parent + "/" + stem + " (" + std::to_string(n) + ")" + ext;
        if (::lstat(candidate.c_str(), &st) != 0) return candidate;
    }
    return desired;  // caller will fail on the collision, which is the honest outcome
}

bool verifyUnchanged(const std::string& path, uint64_t size, int64_t mtime, std::string& err) {
    struct stat st {};
    if (!statFile(path, st)) {
        err = std::string("gone: ") + std::strerror(errno);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        err = "no longer a regular file";
        return false;
    }
    if (static_cast<uint64_t>(st.st_size) != size) {
        err = "size changed since the scan";
        return false;
    }
    if (static_cast<int64_t>(st.st_mtime) != mtime) {
        err = "modified since the scan";
        return false;
    }
    return true;
}

ActionQueue::~ActionQueue() {
    cancel();
    if (thread_.joinable()) thread_.join();
}

void ActionQueue::start(std::vector<ActionItem> items, ActionKind kind,
                        std::string quarantineRoot, Log* log) {
    if (running_.load()) return;
    if (thread_.joinable()) thread_.join();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        progress_ = ActionProgress {};
        progress_.running = true;
        progress_.kind = kind;
        progress_.total = items.size();
        summaryReady_ = false;
        summary_ = RunSummary {};
    }

    cancel_.store(false);
    running_.store(true);
    thread_ = std::thread(&ActionQueue::run, this, std::move(items), kind,
                          std::move(quarantineRoot), log);
}

void ActionQueue::cancel() { cancel_.store(true); }

bool ActionQueue::running() const { return running_.load(); }

ActionProgress ActionQueue::progress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return progress_;
}

bool ActionQueue::takeSummary(RunSummary& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!summaryReady_) return false;
    out = std::move(summary_);
    summary_ = RunSummary {};
    summaryReady_ = false;
    return true;
}

void ActionQueue::run(std::vector<ActionItem> items, ActionKind kind,
                      std::string quarantineRoot, Log* log) {
    RunSummary summary;
    summary.kind = kind;

    uint64_t plannedBytes = 0;
    for (const auto& item : items) plannedBytes += item.size;
    if (log) {
        log->warn(std::string(actionKindName(kind)) + " run started: " +
                  formatCount(items.size()) + " file(s), " + formatSize(plannedBytes) +
                  (kind == ActionKind::Quarantine ? " -> " + quarantineRoot : ""));
    }

    ManifestWriter manifest;
    const std::string manifestPath = newManifestPath(kind, quarantineRoot, timestampNow());
    if (!manifest.open(manifestPath, kind, quarantineRoot)) {
        // Without a manifest a quarantine is not reversible and a delete leaves
        // no record, so neither runs.
        std::lock_guard<std::mutex> lock(mutex_);
        ActionRow row;
        row.state = ActionState::Failed;
        row.error = "cannot write the run manifest at " + manifestPath;
        if (log) log->error(row.error + ", so nothing was touched");
        progress_.failures.push_back(row);
        progress_.failed = 1;
        progress_.running = false;
        summary.failed = 1;
        summary_ = std::move(summary);
        summaryReady_ = true;
        running_.store(false);
        return;
    }
    summary.manifest = manifestPath;

    for (const auto& item : items) {
        if (cancel_.load()) {
            summary.cancelled = true;
            if (log) log->warn("stopped by hand, the rest of the queue was left alone");
            break;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            progress_.current = item.path;
        }

        ActionRow row;
        row.item = item;
        std::string err;

        // The keeper is checked first. If the copy that is supposed to survive
        // is not there any more, nothing else in this row is safe to do. An
        // empty keeper means a uniques run, where by definition there is no
        // other copy; only the reversible move is offered for those, and the
        // interface refuses the delete.
        if (!item.keeper.empty() &&
            !verifyUnchanged(item.keeper, item.keeperSize, item.keeperMtime, err)) {
            row.state = ActionState::Failed;
            row.error = "the copy being kept has changed (" + err + ")";
        } else if (!verifyUnchanged(item.path, item.size, item.mtime, err)) {
            row.state = ActionState::Failed;
            row.error = err;
        } else if (kind == ActionKind::Delete) {
            if (!manifest.append(item.path, {}, item.size, item.mtime, item.hash)) {
                row.state = ActionState::Failed;
                row.error = "could not record the removal, so nothing was removed";
            } else if (::unlink(item.path.c_str()) != 0) {
                row.state = ActionState::Failed;
                row.error = std::strerror(errno);
            } else {
                row.state = ActionState::Done;
            }
        } else {
            const std::string dest = uniquePath(quarantineDest(quarantineRoot, item.path));
            if (!ensureDir(fs::path(dest).parent_path().string())) {
                row.state = ActionState::Failed;
                row.error = "cannot create " + fs::path(dest).parent_path().string();
            } else if (!manifest.append(item.path, dest, item.size, item.mtime, item.hash)) {
                row.state = ActionState::Failed;
                row.error = "could not record the move, so nothing was moved";
            } else if (!moveFile(item.path, dest, err)) {
                row.state = ActionState::Failed;
                row.error = err;
            } else {
                row.state = ActionState::Done;
                row.dest = dest;
            }
        }

        // Written before the counters, so the log reads in the order the files
        // were actually touched, and every path that went is named.
        if (log) {
            if (row.state != ActionState::Done) {
                log->error("failed: " + item.path + "  (" + row.error + ")");
            } else {
                const std::string why =
                    item.keeper.empty() ? ", no other copy" : ", keeping " + item.keeper;
                log->info((kind == ActionKind::Delete ? "deleted: " + item.path
                                                      : "moved: " + item.path + " -> " + row.dest) +
                          "  (" + formatSize(item.size) + why + ")");
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);
        ++progress_.done;
        if (row.state == ActionState::Done) {
            progress_.bytes += item.size;
            ++summary.done;
            summary.bytes += item.size;
            summary.removed.push_back(item.path);
        } else {
            ++progress_.failed;
            ++summary.failed;
            if (progress_.failures.size() < kMaxFailuresKept) progress_.failures.push_back(row);
        }
    }

    manifest.close();

    if (log) {
        log->warn(std::string(actionKindName(kind)) + " run finished: " +
                  formatCount(summary.done) + " done, " + formatCount(summary.failed) +
                  " failed, " + formatSize(summary.bytes) + " reclaimed");
        log->info("manifest: " + manifestPath);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    progress_.running = false;
    progress_.current.clear();
    summary_ = std::move(summary);
    summaryReady_ = true;
    running_.store(false);
}
