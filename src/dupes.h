#pragma once

#include <cstdint>
#include <string>
#include <vector>

// One file worth comparing. The walk fills in everything up to rootIndex; the
// cascade rows fill in the hashes as the file survives each of them.
struct FileEntry {
    std::string path;  // absolute, no trailing slash
    uint64_t size = 0;
    uint64_t dev = 0;  // from lstat, and with ino the identity used for hardlinks
    uint64_t ino = 0;
    uint64_t nlink = 1;
    int64_t mtime = 0;
    // Which input directory this came from. Priority is the position of that
    // directory in the input list, so reordering the list re-ranks every file at
    // once without touching this vector.
    int rootIndex = 0;

    uint64_t headHash = 0;
    uint64_t fullHash = 0;
    bool headIsFull = false;  // size <= head bytes, so the head hash is the whole file
    bool hashed = false;      // fullHash is valid

    // Other paths that resolve to this same inode, folded in by the hardlink
    // collapse. They cost no extra disk, so they are never actionable.
    std::vector<std::string> alsoLinkedAt;
};

struct Member {
    int fileIndex = 0;     // index into the flat FileEntry vector
    bool selected = true;  // ticked for removal; the keeper is never selected
};

// Files with identical contents. Two or more of them, unless `unique` is set, in
// which case it is the one file that matched nothing.
struct DupGroup {
    uint64_t size = 0;            // every member has this size
    std::vector<Member> members;
    int keeper = 0;               // index within members
    bool userPinned = false;      // keeper chosen by hand, priority no longer moves it
    // Reported by a uniques scan: this file has no copy anywhere in the inputs,
    // so there is no keeper to protect and nothing here is safe to delete.
    bool unique = false;
};

// How a keeper is chosen between members whose input directories rank equally,
// which includes the common case of several copies inside one directory.
enum class TieBreak {
    OldestMtime,
    NewestMtime,
    ShortestPath,
    FewestSegments,
    Alphabetical,
};

const char* tieBreakName(TieBreak t);
extern const char* const kTieBreakNames[5];

// The two walk-level choices that are not per-file predicates, so they are not
// pipeline rows. Everything else that shapes a scan lives in the Pipeline.
struct ScopeFilters {
    bool includeHidden = false;
    bool collapseHardlinks = true;
};
