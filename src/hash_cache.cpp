#include "hash_cache.h"

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "hash.h"
#include "util.h"

namespace fs = std::filesystem;

namespace {

// Entries older than this that were not touched this session are dropped on
// save, so a one-off scan of a since-deleted disk does not live forever.
constexpr int64_t kMaxAgeSeconds = 30 * 24 * 60 * 60;

// v2 is keyed by path rather than by device and inode. A v1 file is simply not
// read: its keys cannot be translated without stat-ing every file it mentions,
// which is the work the cache exists to avoid.
constexpr const char* kHeader = "duplicates-ui hash cache v2";

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == '\t') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

}  // namespace

size_t HashCache::KeyHash::operator()(const Key& k) const {
    const size_t h = static_cast<size_t>(xxhash64(k.path.data(), k.path.size(), k.variant));
    return h;
}

std::string HashCache::defaultPath() { return cacheDir() + "/hashes.tsv"; }

void HashCache::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return;

    std::string line;
    if (!std::getline(in, line) || line != kHeader) return;

    std::lock_guard<std::mutex> lock(mutex_);
    while (std::getline(in, line)) {
        const std::vector<std::string> f = splitTabs(line);
        if (f.size() < 6) continue;

        Key k;
        k.path = unescapeField(f[0]);
        if (k.path.empty()) continue;
        k.variant = std::strtoull(f[1].c_str(), nullptr, 10);

        Value v;
        v.size = std::strtoull(f[2].c_str(), nullptr, 10);
        v.mtime = std::strtoll(f[3].c_str(), nullptr, 10);
        v.hash = std::strtoull(f[4].c_str(), nullptr, 10);
        v.seen = std::strtoll(f[5].c_str(), nullptr, 10);
        map_[std::move(k)] = v;
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
            os << escapeField(k.path) << '\t' << k.variant << '\t' << v.size << '\t' << v.mtime
               << '\t' << v.hash << '\t' << v.seen << '\n';
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

bool HashCache::lookup(const std::string& path, uint64_t variant, uint64_t size, int64_t mtime,
                       uint64_t& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = map_.find(Key {path, variant});
    if (it == map_.end()) {
        ++misses_;
        return false;
    }
    // The cheap check the whole cache rests on: a file whose size or mtime moved
    // is a different file as far as its contents are concerned.
    if (it->second.size != size || it->second.mtime != mtime) {
        map_.erase(it);
        ++stale_;
        ++misses_;
        return false;
    }
    it->second.seen = static_cast<int64_t>(std::time(nullptr));
    out = it->second.hash;
    ++hits_;
    return true;
}

void HashCache::insert(const std::string& path, uint64_t variant, uint64_t size, int64_t mtime,
                       uint64_t hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    map_[Key {path, variant}] =
        Value {size, mtime, hash, static_cast<int64_t>(std::time(nullptr))};
    ++generation_;
}

uint64_t HashCache::generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

size_t HashCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return map_.size();
}
