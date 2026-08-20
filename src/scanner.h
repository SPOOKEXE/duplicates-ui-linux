#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "dupes.h"
#include "pipeline.h"

// What the walk left behind. Every counter here is something the user might
// wonder about when a file they expected does not appear in a group.
struct WalkStats {
    uint64_t filesSeen = 0;
    uint64_t bytesSeen = 0;
    uint64_t skippedHidden = 0;
    uint64_t skippedSmall = 0;
    uint64_t skippedLarge = 0;
    uint64_t skippedSymlink = 0;
    uint64_t skippedFiltered = 0;  // turned away by a glob or regex row
    uint64_t prunedDirs = 0;       // directories an exclude pattern skipped whole
    uint64_t collapsedLinks = 0;
    uint64_t dirErrors = 0;
};

struct WalkHooks {
    const std::atomic<bool>* cancel = nullptr;
    std::function<void(uint64_t filesSeen, const std::string& dir)> onProgress;
    std::function<void(const std::string& message)> onError;
    // Ordinary progress worth writing down: a pruned directory, a folded
    // hardlink. Separate from onError so the log can colour them differently.
    std::function<void(const std::string& message)> onNote;
};

// Tidies the input list: normalises each path, drops duplicates, and drops any
// directory nested inside another one, so no file is walked or reported twice.
// Order is preserved, because order is priority.
std::vector<std::string> normalizeRoots(const std::vector<std::string>& roots);

// Walks every root, in order, applying the scope toggles and the pipeline's
// drop rows. rootIndex on each entry is the index of the root it came from,
// which is its priority.
std::vector<FileEntry> walkRoots(const std::vector<std::string>& roots, const ScopeFilters& scope,
                                 const CompiledPipeline& pipe, const WalkHooks& hooks,
                                 WalkStats& stats);

// Folds paths that share a (dev, ino) down to one representative, recording the
// others in alsoLinkedAt.
//
// This is correctness, not an optimisation. Two hardlinks to one inode occupy
// the disk once, so reporting them as duplicates would offer to reclaim bytes
// that do not exist, and would cost a path the user wanted to keep.
void collapseHardlinks(std::vector<FileEntry>& files, TieBreak tie, WalkStats& stats,
                       const WalkHooks* hooks = nullptr);
