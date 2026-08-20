#include "cascade.h"

#include <algorithm>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "hash.h"
#include "hash_cache.h"
#include "util.h"

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
// single operation every row of the cascade is built from.
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

// One emit per this many bytes read, across all workers. Frequent enough that a
// single huge file still shows movement, rare enough that the UI is not woken
// thousands of times a second.
constexpr uint64_t kPulseBytes = 64u << 20;

// Progress and error reporting shared by the worker threads.
struct Reporter {
    const CascadeHooks& hooks;
    std::mutex mutex;
    CascadeProgress p;
    std::atomic<uint64_t> bytes {0};
    std::atomic<uint64_t> done {0};
    std::atomic<uint64_t> sincePulse {0};
    uint64_t stageBase = 0;   // bytes already read when this row started
    uint64_t tickStep = 64;   // emit every this many finished files

    explicit Reporter(const CascadeHooks& h) : hooks(h) {}

    // bytesTotal is what this row expects to read, and 0 when it reads nothing,
    // in which case the UI falls back to counting files.
    void stage(Stage s, uint64_t total, uint64_t candidates, uint64_t bytesTotal = 0) {
        std::lock_guard<std::mutex> lock(mutex);
        p.stage = s;
        p.total = total;
        p.candidates = candidates;
        p.done = 0;
        p.current.clear();
        p.stageBytesTotal = bytesTotal;
        p.stageBytesDone = 0;
        done.store(0);
        sincePulse.store(0);
        stageBase = bytes.load();
        // A fixed step of 64 leaves a few hundred large files reporting nothing
        // at all, so it scales down with the work rather than being a constant.
        tickStep = total > 3200 ? 64 : std::max<uint64_t>(1, total / 50);
        emitLocked();
    }

    // Called from worker threads, so the emit is rate limited: a callback per
    // hashed file would spend more time in the UI than in the I/O.
    void tick() {
        const uint64_t n = done.fetch_add(1) + 1;
        if (n % tickStep != 0) return;
        std::lock_guard<std::mutex> lock(mutex);
        p.done = n;
        updateBytesLocked();
        emitLocked();
    }

    // Called from inside a file's read loop, so a row spending an hour on one
    // file still reports movement.
    void pulse(uint64_t justRead) {
        if (sincePulse.fetch_add(justRead) + justRead < kPulseBytes) return;
        sincePulse.store(0);
        std::lock_guard<std::mutex> lock(mutex);
        p.done = done.load();
        updateBytesLocked();
        emitLocked();
    }

    void setCurrent(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex);
        p.current = path;
        p.done = done.load();
        updateBytesLocked();
        emitLocked();
    }

    void flush() {
        std::lock_guard<std::mutex> lock(mutex);
        p.done = done.load();
        p.current.clear();
        updateBytesLocked();
        emitLocked();
    }

    void updateBytesLocked() {
        p.bytesRead = bytes.load();
        p.stageBytesDone = p.bytesRead - stageBase;
    }

    void error(const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex);
        if (hooks.onError) hooks.onError(message);
    }

    void note(const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex);
        if (hooks.onNote) hooks.onNote(message);
    }

    void emitLocked() {
        if (hooks.onProgress) hooks.onProgress(p);
    }
};

// Compares a bucket's files byte for byte and returns the sets that genuinely
// match. A hash collision, or a file that changed under the scan, shows up here
// as a split rather than as a wrong group.
std::vector<Bucket> exactSplit(const Bucket& bucket, const std::vector<FileEntry>& files,
                               const CascadeHooks& hooks, Reporter& rep,
                               std::atomic<uint64_t>& errors, const ChunkFn& onChunk,
                               std::vector<char>& unreadable) {
    std::vector<Bucket> out;
    Bucket remaining = bucket;

    while (remaining.size() >= 2) {
        if (hooks.cancel && hooks.cancel->load()) return out;

        const int pivot = remaining[0];
        Bucket match {pivot};
        Bucket rest;

        for (size_t i = 1; i < remaining.size(); ++i) {
            bool err = false;
            rep.setCurrent(files[remaining[i]].path);
            if (sameContents(files[pivot].path, files[remaining[i]].path, hooks.cancel, err,
                             &rep.bytes, &onChunk)) {
                match.push_back(remaining[i]);
            } else if (err) {
                // Unreadable now means it cannot be compared, so it leaves the
                // running entirely rather than being assumed identical.
                errors.fetch_add(1);
                // Distinct indices from distinct threads, so no lock is needed;
                // without this the file would later be reported as having no
                // copy, when the truth is that it could not be read.
                unreadable[remaining[i]] = 1;
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

// "first 65536 bytes" reads better in the log than the bare rule name.
std::string ruleLabel(const Rule& r) {
    if (r.kind == RuleKind::HeadBytes) return "first " + formatSize(r.number);
    if (r.kind == RuleKind::SampledHash) return formatSize(r.number) + " sampled";
    return ruleKindName(r.kind);
}

}  // namespace

const char* stageName(Stage s) {
    switch (s) {
        case Stage::Idle: return "idle";
        case Stage::Walking: return "walking";
        case Stage::Sizing: return "grouping by size";
        case Stage::SameName: return "matching filenames";
        case Stage::SameMtime: return "matching mtimes";
        case Stage::HeadBytes: return "hashing first bytes";
        case Stage::SampledHash: return "sampling contents";
        case Stage::FullHash: return "hashing contents";
        case Stage::ExactCompare: return "comparing bytes";
        case Stage::Grouping: return "building groups";
        case Stage::Done: return "done";
        case Stage::Cancelled: return "cancelled";
    }
    return "?";
}

Stage stageForRule(RuleKind kind) {
    switch (kind) {
        case RuleKind::SameName: return Stage::SameName;
        case RuleKind::SameMtime: return Stage::SameMtime;
        case RuleKind::HeadBytes: return Stage::HeadBytes;
        case RuleKind::SampledHash: return Stage::SampledHash;
        case RuleKind::FullHash: return Stage::FullHash;
        case RuleKind::ExactBytes: return Stage::ExactCompare;
        default: return Stage::Sizing;
    }
}

std::vector<DupGroup> runCascade(std::vector<FileEntry>& files, const CompiledPipeline& pipe,
                                 const CascadeHooks& hooks) {
    Reporter rep(hooks);
    const auto cancelled = [&] { return hooks.cancel && hooks.cancel->load(); };
    // One callback shared by every reader, so progress moves inside a single
    // multi-gigabyte file rather than only when it finishes.
    const ChunkFn onChunk = [&](uint64_t n) { rep.pulse(n); };

    // Files a row could not read. They are neither duplicates nor uniques: what
    // is true of them is unknown, so they are left out of both answers rather
    // than being reported as having no copy.
    std::vector<char> unreadable(files.size(), 0);

    // Size, always first and never a row. Free, because the walk already called
    // lstat, and it is what makes every later row affordable.
    rep.stage(Stage::Sizing, files.size(), files.size());
    std::vector<Bucket> buckets;
    {
        Bucket all(files.size());
        for (size_t i = 0; i < files.size(); ++i) all[i] = static_cast<int>(i);
        buckets = repartition<uint64_t>({all}, [&](int i) { return files[i].size; });
    }
    rep.stage(Stage::Sizing, files.size(), countIn(buckets));
    rep.note("size: " + formatCount(files.size()) + " in -> " + formatCount(countIn(buckets)) +
             " candidates");
    if (cancelled()) return {};

    // One seek and one small read per file, and where nearly every same-size
    // coincidence dies.
    const auto runHeadBytes = [&](uint64_t headSize, uint64_t& errors, uint64_t& cacheHits) {
        std::vector<int> candidates = flatten(buckets);
        uint64_t expected = 0;
        for (int i : candidates) expected += std::min<uint64_t>(files[i].size, headSize);
        rep.stage(Stage::HeadBytes, candidates.size(), candidates.size(), expected);
        std::vector<char> bad(files.size(), 0);
        std::atomic<uint64_t> failed {0}, hits {0};

        parallelFor(candidates.size(), pipe.threads, hooks.cancel, [&](size_t k) {
            const int i = candidates[k];
            FileEntry& f = files[i];
            // A file no larger than the head window is hashed in full right
            // here, so the expensive row can skip it outright.
            f.headIsFull = f.size <= headSize;

            uint64_t h = 0;
            // For those files the cached full hash is also the head hash, so a
            // known small file is answered without touching the disk at all.
            // Checking the cache here rather than only in the full hash row is
            // what makes a repeat scan cheap: most files on a real tree are
            // small ones that never reach it.
            if (f.headIsFull && hooks.cache &&
                hooks.cache->lookup(f.path, 0, f.size, f.mtime, h)) {
                f.headHash = h;
                f.fullHash = h;
                f.hashed = true;
                hits.fetch_add(1);
            } else if (hashHead(f.path, headSize, h)) {
                f.headHash = h;
                if (f.headIsFull) {
                    f.fullHash = h;
                    f.hashed = true;
                    if (hooks.cache) hooks.cache->insert(f.path, 0, f.size, f.mtime, h);
                }
                rep.bytes.fetch_add(std::min<uint64_t>(f.size, headSize));
            } else {
                bad[i] = 1;
                failed.fetch_add(1);
                rep.error(f.path + ": unreadable, excluded from the scan");
            }
            rep.tick();
        });
        rep.flush();
        if (cancelled()) return;

        for (size_t i = 0; i < bad.size(); ++i) {
            if (bad[i]) unreadable[i] = 1;
        }
        for (auto& b : buckets) {
            b.erase(std::remove_if(b.begin(), b.end(), [&](int i) { return bad[i] != 0; }), b.end());
        }
        buckets = repartition<uint64_t>(buckets, [&](int i) { return files[i].headHash; });
        errors = failed.load();
        cacheHits = hits.load();
    };

    // A megabyte of reading in place of a whole file. Every member of a bucket
    // shares its size, because the size row runs first and nothing after it ever
    // merges buckets, so a bucket is never a mix of sampled and full hashes.
    const auto runSampledHash = [&](uint64_t sampleBytes, uint64_t& errors, uint64_t& cacheHits) {
        std::vector<int> candidates = flatten(buckets);
        uint64_t expected = 0;
        for (int i : candidates) expected += std::min<uint64_t>(files[i].size, sampleBytes);
        rep.stage(Stage::SampledHash, candidates.size(), candidates.size(), expected);
        std::vector<char> bad(files.size(), 0);
        std::vector<uint64_t> key(files.size(), 0);
        std::atomic<uint64_t> failed {0}, hits {0};

        parallelFor(candidates.size(), pipe.threads, hooks.cancel, [&](size_t k) {
            const int i = candidates[k];
            FileEntry& f = files[i];
            uint64_t h = 0;

            // A file small enough to have been read end to end was cached as a
            // full hash, so that is the entry to ask for.
            const uint64_t variant = f.size <= sampleBytes ? 0 : sampleBytes;
            if (hooks.cache && hooks.cache->lookup(f.path, variant, f.size, f.mtime, h)) {
                key[i] = h;
                if (variant == 0) {
                    f.fullHash = h;
                    f.hashed = true;
                }
                hits.fetch_add(1);
                rep.tick();
                return;
            }

            rep.setCurrent(f.path);
            bool wholeFile = false;
            if (hashSampled(f.path, f.size, sampleBytes, hooks.cancel, h, wholeFile, &rep.bytes,
                            &onChunk)) {
                key[i] = h;
                // Small enough to have been read in full, so this is the real
                // content hash: record it as one and let a later full hash row
                // skip the file entirely.
                if (wholeFile) {
                    f.fullHash = h;
                    f.hashed = true;
                }
                if (hooks.cache) hooks.cache->insert(f.path, wholeFile ? 0 : sampleBytes, f.size,
                                                     f.mtime, h);
            } else if (!cancelled()) {
                bad[i] = 1;
                failed.fetch_add(1);
                rep.error(f.path + ": unreadable, excluded from the scan");
            }
            rep.tick();
        });
        rep.flush();
        if (cancelled()) return;

        for (size_t i = 0; i < bad.size(); ++i) {
            if (bad[i]) unreadable[i] = 1;
        }
        for (auto& b : buckets) {
            b.erase(std::remove_if(b.begin(), b.end(), [&](int i) { return bad[i] != 0; }), b.end());
        }
        buckets = repartition<uint64_t>(buckets, [&](int i) { return key[i]; });
        errors = failed.load();
        cacheHits = hits.load();
    };

    // The whole file, with the cache standing in for a re-read.
    const auto runFullHash = [&](uint64_t& errors, uint64_t& cacheHits) {
        std::vector<int> candidates;
        for (int i : flatten(buckets)) {
            if (!files[i].hashed) candidates.push_back(i);
        }
        uint64_t expected = 0;
        for (int i : candidates) expected += files[i].size;
        rep.stage(Stage::FullHash, candidates.size(), countIn(buckets), expected);
        std::vector<char> bad(files.size(), 0);
        std::atomic<uint64_t> failed {0}, hits {0};

        parallelFor(candidates.size(), pipe.threads, hooks.cancel, [&](size_t k) {
            const int i = candidates[k];
            const FileEntry& f = files[i];
            uint64_t h = 0;

            if (hooks.cache && hooks.cache->lookup(f.path, 0, f.size, f.mtime, h)) {
                files[i].fullHash = h;
                files[i].hashed = true;
                hits.fetch_add(1);
                rep.tick();
                return;
            }

            // Named before it is read, not after: on a tree of large archives
            // this is the only thing that says which file is taking the hour.
            rep.setCurrent(f.path);
            if (hashFull(f.path, hooks.cancel, h, &rep.bytes, &onChunk)) {
                files[i].fullHash = h;
                files[i].hashed = true;
                if (hooks.cache) hooks.cache->insert(f.path, 0, f.size, f.mtime, h);
            } else if (!cancelled()) {
                bad[i] = 1;
                failed.fetch_add(1);
                rep.error(f.path + ": unreadable, excluded from the scan");
            }
            rep.tick();
        });
        rep.flush();
        if (cancelled()) return;

        for (size_t i = 0; i < bad.size(); ++i) {
            if (bad[i]) unreadable[i] = 1;
        }
        for (auto& b : buckets) {
            b.erase(std::remove_if(b.begin(), b.end(),
                                   [&](int i) { return bad[i] != 0 || !files[i].hashed; }),
                    b.end());
        }
        buckets = repartition<uint64_t>(buckets, [&](int i) { return files[i].fullHash; });
        errors = failed.load();
        cacheHits = hits.load();
    };

    // The only row that can prove a match rather than strongly suggest one.
    const auto runExactCompare = [&](uint64_t& errors) {
        // Each bucket compares its first member against the others, and a pair
        // read costs both files, so a bucket of k identical files of size S
        // reads 2S(k-1). An early mismatch only makes the row finish sooner.
        uint64_t expected = 0;
        for (const auto& b : buckets) {
            if (b.size() >= 2) expected += 2 * files[b[0]].size * (b.size() - 1);
        }
        rep.stage(Stage::ExactCompare, buckets.size(), countIn(buckets), expected);
        std::vector<std::vector<Bucket>> perBucket(buckets.size());
        std::atomic<uint64_t> failed {0};

        parallelFor(buckets.size(), pipe.threads, hooks.cancel, [&](size_t k) {
            perBucket[k] = exactSplit(buckets[k], files, hooks, rep, failed, onChunk, unreadable);
            rep.tick();
        });
        rep.flush();
        if (cancelled()) return;

        std::vector<Bucket> merged;
        for (auto& part : perBucket) {
            for (auto& b : part) merged.push_back(std::move(b));
        }
        buckets = std::move(merged);
        errors = failed.load();
    };

    for (const auto& rule : pipe.splits) {
        if (buckets.empty() || cancelled()) break;

        const uint64_t before = countIn(buckets);
        uint64_t errors = 0, cacheHits = 0;

        switch (rule.kind) {
            case RuleKind::SameName:
                rep.stage(Stage::SameName, before, before);
                buckets = repartition<std::string>(
                    buckets, [&](int i) { return basenameOf(files[i].path); });
                break;
            case RuleKind::SameMtime:
                rep.stage(Stage::SameMtime, before, before);
                buckets = repartition<int64_t>(buckets, [&](int i) { return files[i].mtime; });
                break;
            case RuleKind::HeadBytes: runHeadBytes(rule.number, errors, cacheHits); break;
            case RuleKind::SampledHash: runSampledHash(rule.number, errors, cacheHits); break;
            case RuleKind::FullHash: runFullHash(errors, cacheHits); break;
            case RuleKind::ExactBytes: runExactCompare(errors); break;
            default: continue;  // drop rows were applied during the walk
        }
        if (cancelled()) return {};

        const uint64_t after = countIn(buckets);
        rep.stage(stageForRule(rule.kind), 0, after);

        std::string line = ruleLabel(rule) + ": " + formatCount(before) + " in -> " +
                           formatCount(after) + " candidates";
        if (cacheHits > 0) line += ", " + formatCount(cacheHits) + " from cache";
        if (errors > 0) line += ", " + formatCount(errors) + " unreadable";
        rep.note(line);
    }

    rep.stage(Stage::Grouping, buckets.size(), countIn(buckets));

    if (pipe.report == ReportMode::Uniques) {
        // Every file that entered either survived into a bucket of two or more,
        // or was dropped alone by some row. The second set is the answer, and it
        // is easier to take by subtraction than by instrumenting every drop.
        std::vector<char> paired(files.size(), 0);
        for (const auto& b : buckets) {
            if (b.size() < 2) continue;
            for (int i : b) paired[i] = 1;
        }

        std::vector<int> alone;
        for (size_t i = 0; i < files.size(); ++i) {
            if (!paired[i] && !unreadable[i]) alone.push_back(static_cast<int>(i));
        }
        std::sort(alone.begin(), alone.end(), [&](int x, int y) {
            if (files[x].rootIndex != files[y].rootIndex) {
                return files[x].rootIndex < files[y].rootIndex;
            }
            return files[x].path < files[y].path;
        });

        std::vector<DupGroup> lonely;
        lonely.reserve(alone.size());
        for (int i : alone) {
            DupGroup g;
            g.size = files[i].size;
            g.unique = true;
            // Selected by default like any other row, but there is no keeper
            // behind it: the interface refuses to delete these, and only offers
            // the reversible move.
            g.members.push_back(Member {i, true});
            lonely.push_back(std::move(g));
        }
        rep.note("uniques: " + formatCount(lonely.size()) + " file(s) with no copy in the inputs");
        return lonely;
    }

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
