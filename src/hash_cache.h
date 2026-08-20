#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

// Remembers a file's full hash so an unchanged file is never read twice, across
// runs as well as within one. The key is the file's whole identity, so a file
// that has been touched simply misses and is recomputed; there is no
// invalidation logic to get wrong.
//
// The cache is advisory. Deleting the file on disk costs time and nothing else.
class HashCache {
public:
    // Missing or malformed files are not an error: the cache just starts empty.
    void load(const std::string& path);

    // Rewrites the file, keeping entries used this session plus entries seen
    // within the last 30 days, so it cannot grow without bound.
    bool save(const std::string& path);

    bool lookup(uint64_t dev, uint64_t ino, uint64_t size, int64_t mtime, uint64_t& out);
    void insert(uint64_t dev, uint64_t ino, uint64_t size, int64_t mtime, uint64_t hash);

    size_t size() const;
    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }

    static std::string defaultPath();

private:
    struct Key {
        uint64_t dev, ino, size;
        int64_t mtime;
        bool operator==(const Key& o) const {
            return dev == o.dev && ino == o.ino && size == o.size && mtime == o.mtime;
        }
    };

    struct KeyHash {
        size_t operator()(const Key& k) const;
    };

    struct Value {
        uint64_t hash = 0;
        int64_t seen = 0;  // unix time this entry was last useful
    };

    mutable std::mutex mutex_;
    std::unordered_map<Key, Value, KeyHash> map_;
    uint64_t hits_ = 0;
    uint64_t misses_ = 0;
};
