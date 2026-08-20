#include "hash_cache.h"

#include <cinttypes>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "hash.h"
#include "util.h"

namespace fs = std::filesystem;

namespace {

// Entries older than this that were not touched this session are dropped on
// save, so a one-off scan of a since-deleted disk does not live forever.
constexpr int64_t kMaxAgeSeconds = 30 * 24 * 60 * 60;

constexpr const char* kHeader = "duplicates-ui hash cache v1";

}  // namespace

size_t HashCache::KeyHash::operator()(const Key& k) const {
    // The four fields are hashed together rather than combined by hand: inode
    // numbers on one device are sequential, and a weak mix would bucket badly.
    const uint64_t parts[4] = {k.dev, k.ino, k.size, static_cast<uint64_t>(k.mtime)};
    return static_cast<size_t>(xxhash64(parts, sizeof(parts)));
}

std::string HashCache::defaultPath() { return cacheDir() + "/hashes.tsv"; }

void HashCache::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return;

    std::string line;
    if (!std::getline(in, line) || line != kHeader) return;

    std::lock_guard<std::mutex> lock(mutex_);
    while (std::getline(in, line)) {
        Key k {};
        Value v {};
        if (std::sscanf(line.c_str(), "%" SCNu64 "\t%" SCNu64 "\t%" SCNu64 "\t%" SCNd64 "\t%" SCNu64 "\t%" SCNd64,
                        &k.dev, &k.ino, &k.size, &k.mtime, &v.hash, &v.seen) == 6) {
            map_[k] = v;
        }
    }
}

bool HashCache::save(const std::string& path) {
    if (!ensureDir(fs::path(path).parent_path().string())) return false;

    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    std::ostringstream os;
    os << kHeader << '\n';
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [k, v] : map_) {
            if (now - v.seen > kMaxAgeSeconds) continue;
            os << k.dev << '\t' << k.ino << '\t' << k.size << '\t' << k.mtime << '\t' << v.hash
               << '\t' << v.seen << '\n';
        }
    }

    // Same write-then-rename as the session file: a crash mid-write must not
    // leave a truncated cache that then fails to parse.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        out << os.str();
        if (!out) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

bool HashCache::lookup(uint64_t dev, uint64_t ino, uint64_t size, int64_t mtime, uint64_t& out) {
    const Key k {dev, ino, size, mtime};
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = map_.find(k);
    if (it == map_.end()) {
        ++misses_;
        return false;
    }
    it->second.seen = static_cast<int64_t>(std::time(nullptr));
    out = it->second.hash;
    ++hits_;
    return true;
}

void HashCache::insert(uint64_t dev, uint64_t ino, uint64_t size, int64_t mtime, uint64_t hash) {
    const Key k {dev, ino, size, mtime};
    std::lock_guard<std::mutex> lock(mutex_);
    map_[k] = Value {hash, static_cast<int64_t>(std::time(nullptr))};
}

size_t HashCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return map_.size();
}
