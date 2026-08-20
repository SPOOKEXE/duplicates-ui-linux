#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

// xxHash64, implemented here rather than fetched: it is under a hundred lines,
// and the disk is the bottleneck at every size this tool reads.
uint64_t xxhash64(const void* data, size_t len, uint64_t seed = 0);

// Streaming form, for files too large to hold in memory.
class XxHash64 {
public:
    XxHash64() { reset(); }
    void reset(uint64_t seed = 0);
    void update(const void* data, size_t len);
    uint64_t digest() const;

private:
    uint64_t seed_ = 0;
    uint64_t v1_ = 0, v2_ = 0, v3_ = 0, v4_ = 0;
    uint64_t total_ = 0;
    unsigned char buf_[32] = {};
    size_t bufLen_ = 0;
};

// Hashes the first `limit` bytes. Returns false on any read error, which is how
// an unreadable file stays out of every group and so can never be deleted.
bool hashHead(const std::string& path, uint64_t limit, uint64_t& out);

// Hashes the whole file. `cancel` is polled once per buffer, so cancelling
// during a multi-gigabyte read takes effect immediately rather than at EOF.
// bytesRead, when given, is incremented by however much was read.
bool hashFull(const std::string& path, const std::atomic<bool>* cancel, uint64_t& out,
              std::atomic<uint64_t>* bytesRead = nullptr);

// True when both files hold exactly the same bytes, stopping at the first
// difference. `error` is set if either file could not be read to the end, in
// which case the result is false and the pair must not be treated as a match.
bool sameContents(const std::string& a, const std::string& b, const std::atomic<bool>* cancel,
                  bool& error, std::atomic<uint64_t>* bytesRead = nullptr);
