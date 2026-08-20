#include "hash.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

namespace {

constexpr uint64_t kP1 = 11400714785074694791ULL;
constexpr uint64_t kP2 = 14029467366897019727ULL;
constexpr uint64_t kP3 = 1609587929392839161ULL;
constexpr uint64_t kP4 = 9650029242287828579ULL;
constexpr uint64_t kP5 = 2870177450012600261ULL;

// Read buffer for whole-file work. Large enough that the syscall overhead
// disappears, small enough that a dozen hashing threads stay cheap.
constexpr size_t kChunk = 1u << 20;

inline uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

// memcpy rather than a cast: the source pointer has no alignment guarantee.
inline uint64_t read64(const unsigned char* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

inline uint32_t read32(const unsigned char* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

inline uint64_t round64(uint64_t acc, uint64_t input) {
    acc += input * kP2;
    acc = rotl(acc, 31);
    acc *= kP1;
    return acc;
}

inline uint64_t mergeRound(uint64_t acc, uint64_t val) {
    val = round64(0, val);
    acc ^= val;
    return acc * kP1 + kP4;
}

inline uint64_t avalanche(uint64_t h) {
    h ^= h >> 33;
    h *= kP2;
    h ^= h >> 29;
    h *= kP3;
    h ^= h >> 32;
    return h;
}

// The tail shared by the one-shot and the streaming form: whatever is left once
// every full 32-byte block has been folded in.
uint64_t finalize(uint64_t h, const unsigned char* p, size_t len) {
    const unsigned char* end = p + len;
    while (p + 8 <= end) {
        h ^= round64(0, read64(p));
        h = rotl(h, 27) * kP1 + kP4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= static_cast<uint64_t>(read32(p)) * kP1;
        h = rotl(h, 23) * kP2 + kP3;
        p += 4;
    }
    while (p < end) {
        h ^= static_cast<uint64_t>(*p) * kP5;
        h = rotl(h, 11) * kP1;
        ++p;
    }
    return avalanche(h);
}

// Opens for a sequential full read and tells the kernel so, which is worth a
// measurable amount on rotational media.
int openSequential(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
#ifdef POSIX_FADV_SEQUENTIAL
    if (fd >= 0) ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
    return fd;
}

// read() can return short for reasons that are not EOF, and must be retried on
// EINTR. Returns bytes read, or -1 on a real error.
ssize_t readFull(int fd, unsigned char* buf, size_t want) {
    size_t got = 0;
    while (got < want) {
        const ssize_t n = ::read(fd, buf + got, want - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;
        got += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(got);
}

}  // namespace

uint64_t xxhash64(const void* data, size_t len, uint64_t seed) {
    const unsigned char* const start = static_cast<const unsigned char*>(data);
    const unsigned char* p = start;
    uint64_t h;

    if (len >= 32) {
        const unsigned char* const limit = start + len - 32;
        uint64_t v1 = seed + kP1 + kP2, v2 = seed + kP2, v3 = seed, v4 = seed - kP1;
        do {
            v1 = round64(v1, read64(p)); p += 8;
            v2 = round64(v2, read64(p)); p += 8;
            v3 = round64(v3, read64(p)); p += 8;
            v4 = round64(v4, read64(p)); p += 8;
        } while (p <= limit);
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = mergeRound(h, v1);
        h = mergeRound(h, v2);
        h = mergeRound(h, v3);
        h = mergeRound(h, v4);
    } else {
        h = seed + kP5;
    }

    h += static_cast<uint64_t>(len);
    return finalize(h, p, len - static_cast<size_t>(p - start));
}

void XxHash64::reset(uint64_t seed) {
    seed_ = seed;
    v1_ = seed + kP1 + kP2;
    v2_ = seed + kP2;
    v3_ = seed;
    v4_ = seed - kP1;
    total_ = 0;
    bufLen_ = 0;
}

void XxHash64::update(const void* data, size_t len) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    const unsigned char* const end = p + len;
    total_ += len;

    // Not enough for a block even with what is already held back: just hold it.
    if (bufLen_ + len < 32) {
        std::memcpy(buf_ + bufLen_, p, len);
        bufLen_ += len;
        return;
    }

    if (bufLen_ > 0) {
        const size_t need = 32 - bufLen_;
        std::memcpy(buf_ + bufLen_, p, need);
        const unsigned char* b = buf_;
        v1_ = round64(v1_, read64(b)); b += 8;
        v2_ = round64(v2_, read64(b)); b += 8;
        v3_ = round64(v3_, read64(b)); b += 8;
        v4_ = round64(v4_, read64(b));
        p += need;
        bufLen_ = 0;
    }

    if (p + 32 <= end) {
        const unsigned char* const limit = end - 32;
        do {
            v1_ = round64(v1_, read64(p)); p += 8;
            v2_ = round64(v2_, read64(p)); p += 8;
            v3_ = round64(v3_, read64(p)); p += 8;
            v4_ = round64(v4_, read64(p)); p += 8;
        } while (p <= limit);
    }

    if (p < end) {
        bufLen_ = static_cast<size_t>(end - p);
        std::memcpy(buf_, p, bufLen_);
    }
}

uint64_t XxHash64::digest() const {
    uint64_t h;
    if (total_ >= 32) {
        h = rotl(v1_, 1) + rotl(v2_, 7) + rotl(v3_, 12) + rotl(v4_, 18);
        h = mergeRound(h, v1_);
        h = mergeRound(h, v2_);
        h = mergeRound(h, v3_);
        h = mergeRound(h, v4_);
    } else {
        h = seed_ + kP5;
    }
    h += total_;
    return finalize(h, buf_, bufLen_);
}

bool hashHead(const std::string& path, uint64_t limit, uint64_t& out) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    std::vector<unsigned char> buf(static_cast<size_t>(std::min<uint64_t>(limit, kChunk)));
    XxHash64 h;
    uint64_t left = limit;
    bool ok = true;

    while (left > 0 && !buf.empty()) {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(left, buf.size()));
        const ssize_t n = readFull(fd, buf.data(), want);
        if (n < 0) {
            ok = false;
            break;
        }
        if (n == 0) break;  // file is shorter than the limit, which is fine
        h.update(buf.data(), static_cast<size_t>(n));
        left -= static_cast<uint64_t>(n);
    }

    ::close(fd);
    if (ok) out = h.digest();
    return ok;
}

bool hashFull(const std::string& path, const std::atomic<bool>* cancel, uint64_t& out,
              std::atomic<uint64_t>* bytesRead, const ChunkFn* onChunk) {
    const int fd = openSequential(path);
    if (fd < 0) return false;

    std::vector<unsigned char> buf(kChunk);
    XxHash64 h;
    bool ok = true;

    for (;;) {
        if (cancel && cancel->load()) {
            ok = false;
            break;
        }
        const ssize_t n = readFull(fd, buf.data(), buf.size());
        if (n < 0) {
            ok = false;
            break;
        }
        if (n == 0) break;
        h.update(buf.data(), static_cast<size_t>(n));
        if (bytesRead) bytesRead->fetch_add(static_cast<uint64_t>(n));
        if (onChunk) (*onChunk)(static_cast<uint64_t>(n));
    }

    ::close(fd);
    if (ok) out = h.digest();
    return ok;
}

bool hashSampled(const std::string& path, uint64_t size, uint64_t sampleBytes,
                 const std::atomic<bool>* cancel, uint64_t& out, bool& wholeFile,
                 std::atomic<uint64_t>* bytesRead, const ChunkFn* onChunk) {
    wholeFile = false;
    if (sampleBytes == 0) return false;

    // Reading the whole thing is both cheaper and stronger than seeking around
    // inside it, so a file that fits in the budget is simply hashed in full.
    if (size <= sampleBytes) {
        wholeFile = true;
        return hashFull(path, cancel, out, bytesRead, onChunk);
    }

    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
#ifdef POSIX_FADV_RANDOM
    ::posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);
#endif

    const uint64_t window = std::max<uint64_t>(1, sampleBytes / kSampleWindows);
    // Seeded with the size so two files of different lengths can never collide
    // here even if every window they share happens to agree.
    XxHash64 h;
    h.update(&size, sizeof(size));

    std::vector<unsigned char> buf(static_cast<size_t>(window));
    bool ok = true;

    for (int i = 0; i < kSampleWindows; ++i) {
        if (cancel && cancel->load()) {
            ok = false;
            break;
        }
        // Spread so the first window starts at byte zero and the last one ends
        // at the final byte: a difference in the tail is the common case an
        // append-only archive produces.
        const uint64_t span = size - window;
        const uint64_t offset = span * static_cast<uint64_t>(i) /
                                static_cast<uint64_t>(kSampleWindows - 1);

        size_t got = 0;
        while (got < buf.size()) {
            const ssize_t n = ::pread(fd, buf.data() + got, buf.size() - got,
                                      static_cast<off_t>(offset + got));
            if (n < 0) {
                if (errno == EINTR) continue;
                ok = false;
                break;
            }
            if (n == 0) break;
            got += static_cast<size_t>(n);
        }
        if (!ok) break;

        h.update(buf.data(), got);
        if (bytesRead) bytesRead->fetch_add(static_cast<uint64_t>(got));
        if (onChunk) (*onChunk)(static_cast<uint64_t>(got));
    }

    ::close(fd);
    if (ok) out = h.digest();
    return ok;
}

bool sameContents(const std::string& a, const std::string& b, const std::atomic<bool>* cancel,
                  bool& error, std::atomic<uint64_t>* bytesRead, const ChunkFn* onChunk) {
    error = false;
    const int fa = openSequential(a);
    if (fa < 0) {
        error = true;
        return false;
    }
    const int fb = openSequential(b);
    if (fb < 0) {
        ::close(fa);
        error = true;
        return false;
    }

    std::vector<unsigned char> bufA(kChunk), bufB(kChunk);
    bool same = true;

    for (;;) {
        if (cancel && cancel->load()) {
            error = true;
            same = false;
            break;
        }
        const ssize_t na = readFull(fa, bufA.data(), bufA.size());
        const ssize_t nb = readFull(fb, bufB.data(), bufB.size());
        if (na < 0 || nb < 0) {
            error = true;
            same = false;
            break;
        }
        // A short read on one side only means the files differ in length, which
        // the caller already ruled out, so treat it as a mismatch rather than
        // silently declaring a prefix match.
        if (na != nb) {
            same = false;
            break;
        }
        if (na == 0) break;
        if (bytesRead) bytesRead->fetch_add(static_cast<uint64_t>(na) * 2);
        if (onChunk) (*onChunk)(static_cast<uint64_t>(na) * 2);
        if (std::memcmp(bufA.data(), bufB.data(), static_cast<size_t>(na)) != 0) {
            same = false;
            break;
        }
    }

    ::close(fa);
    ::close(fb);
    return same;
}
