#pragma once

#include <cstdint>
#include <string>
#include <vector>

// What was done to the files a run touched.
enum class ActionKind { Delete, Quarantine };

const char* actionKindName(ActionKind k);

// One past run, read back from its manifest.
struct RunEntry {
    std::string manifest;  // path to the manifest file
    std::string stamp;     // 2026-08-21T14-02-33
    ActionKind kind = ActionKind::Quarantine;
    std::string root;      // quarantine root, empty for delete runs
    uint64_t files = 0;
    uint64_t bytes = 0;
    bool restored = false;
};

// Delete runs are logged in the state directory; they cannot be undone, but a
// record of what went is worth having. Quarantine manifests live with the files
// they describe, so moving the quarantine folder keeps them together.
std::string deleteRunsDir();
std::string quarantineRunsDir(const std::string& quarantineRoot);
std::string newManifestPath(ActionKind kind, const std::string& quarantineRoot,
                            const std::string& stamp);

// Appends one line per file and flushes it before the caller touches the source,
// so a crash can leave a file recorded but not moved, and never the reverse.
class ManifestWriter {
public:
    ~ManifestWriter();

    bool open(const std::string& path, ActionKind kind, const std::string& root);
    bool append(const std::string& original, const std::string& dest, uint64_t size, int64_t mtime,
                uint64_t hash);
    void markRestored();
    void close();

    bool isOpen() const { return file_ != nullptr; }
    const std::string& path() const { return path_; }

private:
    std::string path_;
    void* file_ = nullptr;  // FILE*, kept opaque so the header stays clean
};

// Reads every manifest from the state directory and, when one is configured,
// from the quarantine folder. Newest first.
std::vector<RunEntry> loadRuns(const std::string& quarantineRoot);

struct RestoreResult {
    uint64_t restored = 0;
    uint64_t skipped = 0;  // the original path is occupied again
    uint64_t failed = 0;
    std::vector<std::string> notes;
};

// Puts every file in a quarantine run back where it came from. A path that has
// since been reoccupied is reported and left alone, never overwritten.
RestoreResult restoreRun(const RunEntry& run);
