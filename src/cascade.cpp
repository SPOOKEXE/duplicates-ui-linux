#include "cascade.h"

#include <algorithm>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "hash.h"
#include "hash_cache.h"

namespace {

using Bucket = std::vector<int>;

// Runs body(0..n-1) across a thread pool, polling cancel between items. body
// must be safe to call from several threads at once.
void parallelFor(size_t n, int threads, const std::atomic<bool>* cancel,
                 const std::function<void(size_t)>& body) {
    if (n == 0) return;
    const int count = std::max(1, std::min<int>(threads, static_cast<int>(n)));
    std::atomic<size_t> next {0};

    const auto run = [&] {
        for (;;) {
            if (cancel && cancel->load()) return;
            const size_t i = next.fetch_add(1);
            if (i >= n) return;
            body(i);
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(count - 1));
    for (int k = 1; k < count; ++k) pool.emplace_back(run);
    run();  // the calling thread pulls its share rather than idling
    for (auto& t : pool) t.join();
}

// Splits every bucket by a key and drops whatever is left alone, which is the
// single operation every stage of the cascade is built from.
template <typename Key, typename KeyFn>
std::vector<Bucket> repartition(const std::vector<Bucket>& in, KeyFn key) {
    std::vector<Bucket> out;
    std::unordered_map<Key, Bucket> split;
    for (const auto& bucket : in) {
        split.clear();
        for (int idx : bucket) split[key(idx)].push_back(idx);
        for (auto& entry : split) {
            if (entry.second.size() >= 2) out.push_back(std::move(entry.second));
        }
    }
    return out;
}

uint64_t countIn(const std::vector<Bucket>& buckets) {
    uint64_t n = 0;
    for (const auto& b : buckets) n += b.size();
    return n;
}

std::vector<int> flatten(const std::vector<Bucket>& buckets) {
    std::vector<int> out;
    out.reserve(countIn(buckets));
    for (const auto& b : buckets) out.insert(out.end(), b.begin(), b.end());
    return out;
}

std::string basenameOf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Progress and error reporting shared by the worker threads.
struct Reporter {
    const CascadeHooks& hooks;
    std::mutex mutex;
    CascadeProgress p;
    std::atomic<uint64_t> bytes {0};
    std::atomic<uint64_t> done {0};

    explicit Reporter(const CascadeHooks& h) : hooks(h) {}

    void stage(Stage s, uint64_t total, uint64_t candidates) {
        std::lock_guard<std::mutex> lock(mutex);
        p.stage = s;
        p.total = total;
        p.candidates = candidates;
        p.done = 0;
        done.store(0);
        emitLocked();
    }

    // Called from worker threads, so the emit is rate limited: a callback per
    // hashed file would spend more time in the UI than in the I/O.
    void tick() {
        const uint64_t n = done.fetch_add(1) + 1;
        if (n % 64 != 0) return;
        std::lock_guard<std::mutex> lock(mutex);
        p.done = n;
        p.bytesRead = bytes.load();
        emitLocked();
    }

    void flush() {
        std::lock_guard<std::mutex> lock(mutex);
        p.done = done.load();
        p.bytesRead = bytes.load();
        emitLocked();
    }

    void error(const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex);
        if (hooks.onError) hooks.onError(message);
    }

    void emitLocked() {
        if (hooks.onProgress) hooks.onProgress(p);
    }
};

// Compares a bucket's files byte for byte and returns the sets that genuinely
// match. A hash collision, or a file that changed under the scan, shows up here
// as a split rather than as a wrong group.
std::vector<Bucket> exactSplit(const Bucket& bucket, const std::vector<FileEntry>& files,
                               const CascadeHooks& hooks, Reporter& rep) {
    std::vector<Bucket> out;
    Bucket remaining = bucket;

    while (remaining.size() >= 2) {
        if (hooks.cancel && hooks.cancel->load()) return out;

        const int pivot = remaining[0];
        Bucket match {pivot};
        Bucket rest;

        for (size_t i = 1; i < remaining.size(); ++i) {
            bool err = false;
            if (sameContents(files[pivot].path, files[remaining[i]].path, hooks.cancel, err,
                             &rep.bytes)) {
                match.push_back(remaining[i]);
            } else if (err) {
                // Unreadable now means it cannot be compared, so it leaves the
                // running entirely rather than being assumed identical.
                rep.error(files[remaining[i]].path + ": unreadable during byte comparison");
            } else {
                rest.push_back(remaining[i]);
            }
        }

        if (match.size() >= 2) out.push_back(std::move(match));
        remaining = std::move(rest);
    }
    return out;
}

}  // namespace

const char* stageName(Stage s) {
    switch (s) {
        case Stage::Idle: return "idle";
        case Stage::Walking: return "walking";
        case Stage::Sizing: return "grouping by size";
        case Stage::HeadBytes: return "hashing first bytes";
        case Stage::FullHash: return "hashing contents";
        case Stage::ExactCompare: return "comparing bytes";
        case Stage::Grouping: return "building groups";
        case Stage::Done: return "done";
        case Stage::Cancelled: return "cancelled";
    }
    return "?";
}

std::vector<DupGroup> runCascade(std::vector<FileEntry>& files, const StageSettings& st,
                                 const CascadeHooks& hooks) {
    Reporter rep(hooks);
    const auto cancelled = [&] { return hooks.cancel && hooks.cancel->load(); };

    // Stage 1: size. Free, because the walk already called lstat, and it is what
    // makes every later stage affordable.
    rep.stage(Stage::Sizing, files.size(), files.size());
    std::vector<Bucket> buckets;
    {
        Bucket all(files.size());
        for (size_t i = 0; i < files.size(); ++i) all[i] = static_cast<int>(i);
        buckets = repartition<uint64_t>({all}, [&](int i) { return files[i].size; });
    }
    // These two only ever split a bucket further, so they can produce a false
    // negative but never a false positive, which is why they are plain toggles.
    if (st.sameName) {
        buckets = repartition<std::string>(buckets, [&](int i) { return basenameOf(files[i].path); });
    }
    if (st.sameMtime) {
        buckets = repartition<int64_t>(buckets, [&](int i) { return files[i].mtime; });
    }
    rep.stage(Stage::Sizing, files.size(), countIn(buckets));
    if (cancelled()) return {};

    // Stage 2: the first N bytes. One seek and one small read per file, and it
    // is where nearly every same-size coincidence dies.
    if (st.headBytes && !buckets.empty()) {
        std::vector<int> candidates = flatten(buckets);
        rep.stage(Stage::HeadBytes, candidates.size(), candidates.size());
        std::vector<char> bad(files.size(), 0);

        parallelFor(candidates.size(), hooks.threads, hooks.cancel, [&](size_t k) {
            const int i = candidates[k];
            FileEntry& f = files[i];
            // A file no larger than the head window is hashed in full right
            // here, so the expensive stage can skip it outright.
            f.headIsFull = f.size <= st.headSize;

            uint64_t h = 0;
            // For those files the cached full hash is also the head hash, so a
            // known small file is answered without touching the disk at all.
            // Checking the cache here rather than only in stage 3 is what makes
            // a repeat scan cheap: most files on a real tree are small ones that
            // never reach stage 3.
            if (f.headIsFull && hooks.cache &&
                hooks.cache->lookup(f.dev, f.ino, f.size, f.mtime, h)) {
                f.headHash = h;
                f.fullHash = h;
                f.hashed = true;
            } else if (hashHead(f.path, st.headSize, h)) {
                f.headHash = h;
                if (f.headIsFull) {
                    f.fullHash = h;
                    f.hashed = true;
                    if (hooks.cache) hooks.cache->insert(f.dev, f.ino, f.size, f.mtime, h);
                }
                rep.bytes.fetch_add(std::min<uint64_t>(f.size, st.headSize));
            } else {
                bad[i] = 1;
                rep.error(f.path + ": unreadable, excluded from the scan");
            }
            rep.tick();
        });
        rep.flush();
        if (cancelled()) return {};

        for (auto& b : buckets) {
            b.erase(std::remove_if(b.begin(), b.end(), [&](int i) { return bad[i] != 0; }), b.end());
        }
        buckets = repartition<uint64_t>(buckets, [&](int i) { return files[i].headHash; });
        rep.stage(Stage::HeadBytes, 0, countIn(buckets));
    }

    // Stage 3: the whole file, with the cache standing in for a re-read.
    if (st.fullHash && !buckets.empty()) {
        std::vector<int> candidates;
        for (int i : flatten(buckets)) {
            if (!files[i].hashed) candidates.push_back(i);
        }
        rep.stage(Stage::FullHash, candidates.size(), countIn(buckets));
        std::vector<char> bad(files.size(), 0);

        parallelFor(candidates.size(), hooks.threads, hooks.cancel, [&](size_t k) {
            const int i = candidates[k];
            const FileEntry& f = files[i];
            uint64_t h = 0;

            if (hooks.cache && hooks.cache->lookup(f.dev, f.ino, f.size, f.mtime, h)) {
                files[i].fullHash = h;
                files[i].hashed = true;
            } else if (hashFull(f.path, hooks.cancel, h, &rep.bytes)) {
                files[i].fullHash = h;
                files[i].hashed = true;
                if (hooks.cache) hooks.cache->insert(f.dev, f.ino, f.size, f.mtime, h);
            } else if (!cancelled()) {
                bad[i] = 1;
                rep.error(f.path + ": unreadable, excluded from the scan");
            }
            rep.tick();
        });
        rep.flush();
        if (cancelled()) return {};

        for (auto& b : buckets) {
            b.erase(std::remove_if(b.begin(), b.end(),
                                   [&](int i) { return bad[i] != 0 || !files[i].hashed; }),
                    b.end());
        }
        buckets = repartition<uint64_t>(buckets, [&](int i) { return files[i].fullHash; });
        rep.stage(Stage::FullHash, 0, countIn(buckets));
    }

    // Stage 4: byte for byte. The only stage that can prove a match rather than
    // strongly suggest one, which is why it is on by default.
    if (st.exactCompare && !buckets.empty()) {
        rep.stage(Stage::ExactCompare, buckets.size(), countIn(buckets));
        std::vector<std::vector<Bucket>> perBucket(buckets.size());

        parallelFor(buckets.size(), hooks.threads, hooks.cancel, [&](size_t k) {
            perBucket[k] = exactSplit(buckets[k], files, hooks, rep);
            rep.tick();
        });
        rep.flush();
        if (cancelled()) return {};

        std::vector<Bucket> merged;
        for (auto& part : perBucket) {
            for (auto& b : part) merged.push_back(std::move(b));
        }
        buckets = std::move(merged);
        rep.stage(Stage::ExactCompare, 0, countIn(buckets));
    }

    rep.stage(Stage::Grouping, buckets.size(), countIn(buckets));
    std::vector<DupGroup> groups;
    groups.reserve(buckets.size());
    for (auto& b : buckets) {
        if (b.size() < 2) continue;
        // Sorted so a group reads in priority order, with the likely keeper first.
        std::sort(b.begin(), b.end(), [&](int x, int y) {
            if (files[x].rootIndex != files[y].rootIndex) {
                return files[x].rootIndex < files[y].rootIndex;
            }
            return files[x].path < files[y].path;
        });

        DupGroup g;
        g.size = files[b[0]].size;
        g.members.reserve(b.size());
        for (int i : b) g.members.push_back(Member {i, true});
        groups.push_back(std::move(g));
    }
    return groups;
}
