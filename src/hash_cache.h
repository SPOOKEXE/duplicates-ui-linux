#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

// Remembers a file's hash so an unchanged file is never read twice, across runs
// as well as within one.
//
// Keyed by path, and validated against size and mtime rather than keyed by
// them. The earlier version keyed on (device, inode): correct, but a device
// number is handed out at mount time, so replugging an external disk renumbered
// it and threw away every entry for terabytes of files. A path survives that.
// A rename does not, and costs one re-read, which is the cheaper mistake.
//
// A file whose size or mtime moved simply misses and is recomputed, so there is
// no invalidation logic to get wrong. Writing to a file inside an archive moves
// both, which is the case this exists for.
//
// The cache is advisory. Deleting the file on disk costs time and nothing else.
class HashCache {
public:
    // Missing or malformed files are not an error: the cache just starts empty.
    void load(const std::string& path);

    // Rewrites the file, keeping entries used this session plus entries seen
    // within the last 30 days, so it cannot grow without bound.
    bool save(const std::string& path);

    // `variant` separates hashes of the same file that are not the same number:
    // 0 is the whole file, anything else is the sample size that produced it.
    // Without it a sampled hash would be served up as a full one.
    bool lookup(const std::string& path, uint64_t variant, uint64_t size, int64_t mtime,
                uint64_t& out);
    void insert(const std::string& path, uint64_t variant, uint64_t size, int64_t mtime,
                uint64_t hash);

    size_t size() const;
    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }
    // Entries dropped because the file had changed since it was recorded.
    uint64_t stale() const { return stale_; }

    // Bumped by every insert. A caller saves when this has moved, so a long
    // scan's work survives the app being killed rather than only a clean exit.
    uint64_t generation() const;

    static std::string defaultPath();

private:
    struct Key {
        std::string path;
        uint64_t variant = 0;
        bool operator==(const Key& o) const { return variant == o.variant && path == o.path; }
    };

    struct KeyHash {
        size_t operator()(const Key& k) const;
    };

    struct Value {
        uint64_t size = 0;
        int64_t mtime = 0;
        uint64_t hash = 0;
        int64_t seen = 0;  // unix time this entry was last useful
    };

    mutable std::mutex mutex_;
    std::unordered_map<Key, Value, KeyHash> map_;
    uint64_t hits_ = 0;
    uint64_t misses_ = 0;
    uint64_t stale_ = 0;
    uint64_t generation_ = 0;
};
